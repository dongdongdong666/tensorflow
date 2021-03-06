/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/contrib/tensorrt/convert/convert_nodes.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tensorflow/contrib/tensorrt/convert/utils.h"
#include "tensorflow/contrib/tensorrt/log/trt_logger.h"
#include "tensorflow/contrib/tensorrt/plugin/trt_plugin_factory.h"
#include "tensorflow/contrib/tensorrt/resources/trt_resource_manager.h"
#include "tensorflow/contrib/tensorrt/resources/trt_resources.h"
#include "tensorflow/core/framework/node_def.pb.h"  // NOLINT
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/tensor.pb.h"        // NOLINT
#include "tensorflow/core/framework/tensor_shape.pb.h"  // NOLINT
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/graph/graph_constructor.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/tensor_coding.h"
#include "tensorflow/core/platform/types.h"

#if GOOGLE_CUDA
#if GOOGLE_TENSORRT
#include "tensorrt/include/NvInfer.h"

// Check if the types are equal. Cast to int first so that failure log message
// would work!
#define TFTRT_CHECK_EQ_TYPE(val1, val2) CHECK_EQ((int)val1, (int)val2)

#define TFTRT_INTERNAL_ERROR_AT_NODE(node)                               \
  do {                                                                   \
    return tensorflow::errors::Internal(                                 \
        "TFTRT::", __FUNCTION__, "failed to add TRT layer, at: ", node); \
  } while (0)

#define TFTRT_RETURN_ERROR_IF_FALSE(status, node) \
  do {                                            \
    if (status == false) {                        \
      TFTRT_INTERNAL_ERROR_AT_NODE(node);         \
    }                                             \
  } while (0)

#define TFTRT_RETURN_ERROR_IF_NULLPTR(ptr, node) \
  do {                                           \
    if (ptr == nullptr) {                        \
      TFTRT_INTERNAL_ERROR_AT_NODE(node);        \
    }                                            \
  } while (0)

namespace tensorflow {
namespace tensorrt {
// TODO(aaroey): put these constants into some class.
const char* const kInputPHName = "TensorRTInputPH_";
const char* const kOutputPHName = "TensorRTOutputPH_";

namespace convert {
using ::tensorflow::str_util::Split;
using ::tensorflow::strings::StrAppend;
using ::tensorflow::strings::StrCat;

inline tensorflow::Status ConvertDType(tensorflow::DataType tf_dtype,
                                       nvinfer1::DataType* trt_dtype) {
  switch (tf_dtype) {
    case tensorflow::DataType::DT_FLOAT:
      *trt_dtype = nvinfer1::DataType::kFLOAT;
      break;
    // TODO(aaroey): this should be DT_QINT8 which is not a well supported type.
    case tensorflow::DataType::DT_INT8:
      *trt_dtype = nvinfer1::DataType::kINT8;
      break;
    case tensorflow::DataType::DT_HALF:
      *trt_dtype = nvinfer1::DataType::kHALF;
      break;
    case tensorflow::DataType::DT_INT32:
      *trt_dtype = nvinfer1::DataType::kINT32;
      break;
    default:
      return tensorflow::errors::InvalidArgument(
          "Unsupported data type ", tensorflow::DataTypeString(tf_dtype));
  }
  return tensorflow::Status::OK();
}

void GetOutputProperties(const grappler::GraphProperties& graph_properties,
                         const Node* node, const int out_port,
                         PartialTensorShape* shape,
                         tensorflow::DataType* dtype) {
  if (graph_properties.HasOutputProperties(node->name())) {
    auto output_params = graph_properties.GetOutputProperties(node->name());
    auto out_shape = output_params.at(out_port);
    *dtype = out_shape.dtype();
    *shape = out_shape.shape();
  } else {
    VLOG(0) << "Unknown output shape" << node->name();
    *dtype = node->output_type(out_port);
  }
}

void GetInputProperties(const grappler::GraphProperties& graph_properties,
                        const Node* node, const int in_port,
                        PartialTensorShape* shape,
                        tensorflow::DataType* dtype) {
  if (graph_properties.HasInputProperties(node->name())) {
    auto input_params = graph_properties.GetInputProperties(node->name());
    auto in_shape = input_params.at(in_port);
    *dtype = in_shape.dtype();
    *shape = in_shape.shape();
  } else {
    *dtype = node->input_type(in_port);
  }
}

tensorflow::Status ValidateInputProperties(const PartialTensorShape& shape,
                                           const tensorflow::DataType dtype,
                                           nvinfer1::DataType* trt_dtype) {
  // TODO(aaroey): some of these checks also apply to IsTensorRTCandidate(), so
  // put them there instead.
  TF_RETURN_IF_ERROR(ConvertDType(dtype, trt_dtype));
  if (shape.dims() < 0) {
    return tensorflow::errors::InvalidArgument("Input tensor rank is unknown.");
  }
  if (shape.dims() > 9) {
    return tensorflow::errors::OutOfRange(
        "Input tensor rank is greater than 8.");
  }
  for (int d = 1; d < shape.dims(); ++d) {
    if (shape.dim_size(d) < 0) {
      return tensorflow::errors::InvalidArgument(
          "Input tensor with shape ", shape.DebugString(),
          " has an unknown non-batch dimemension at dim ", d);
    }
  }
  return Status::OK();
}

string DebugString(const nvinfer1::Dims& dims) {
  string out = StrCat("nvinfer1::Dims(nbDims=", dims.nbDims, ", d=");
  for (int i = 0; i < dims.nbDims; ++i) {
    StrAppend(&out, dims.d[i], ",");
  }
  StrAppend(&out, ")");
  return out;
}

string DebugString(const nvinfer1::ITensor& tensor) {
  return StrCat("nvinfer1::ITensor(@", reinterpret_cast<uintptr_t>(&tensor),
                ", shape=", DebugString(tensor.getDimensions()), ")");
}

// Return whether or not the broadcast is feasible;
bool TensorRTGetBroadcastShape(const nvinfer1::Dims& operand_l,
                               const bool operand_l_is_tensor,
                               const nvinfer1::Dims& operand_r,
                               const bool operand_r_is_tensor,
                               nvinfer1::Dims* operand_l_new_shape,
                               nvinfer1::Dims* operand_r_new_shape) {
  // ***************************************************************************
  // TensorRT Elementwise op supports broadcast but requires both tensor to be
  // of Identical rank
  //
  // We consider case of:
  //   1. operand_l to be a Tensor & operand_r to be a Const;
  //   2. operand_l to be a Tensor & operand_r to be a Tensor;
  // note: const op const (constant folding) should fallback to TensorFlow
  //
  // broadcast scheme:
  //       T:  1 3 5    (tensor would not have batch dimension)
  //       W:  1 1 3 1  (weight would have all explicit dimensions)
  // i. fill in explicit dimensions
  //    -> T: -1 1 3 5  (we put a -1 for batch dimension)
  //    -> W:  1 1 3 1
  // ii. compare broadcast feasibility
  //
  // We cannot support the following since TensorRT does not allow manipulation
  // on batch dimension, we cannot generate output with proper shape
  //    T: 3 5 1
  //    W: 1 1 1  1 3 5 1
  // -> T: 1 1 1 -1 3 5 1
  // -> W: 1 1 1  1 3 5 1
  // ***************************************************************************
  const int max_nb_dims = nvinfer1::Dims::MAX_DIMS + 1;
  const size_t element_size = sizeof(operand_l.d[0]);

  // fill in dimensions
  int l_s[max_nb_dims];
  std::fill(l_s, l_s + max_nb_dims, 1);
  int l_d = operand_l_is_tensor ? operand_l.nbDims + 1 : operand_l.nbDims;
  int r_s[max_nb_dims];
  std::fill(r_s, r_s + max_nb_dims, 1);
  int r_d = operand_r_is_tensor ? operand_r.nbDims + 1 : operand_r.nbDims;

  int max_d = std::max(l_d, r_d);
  std::memcpy(l_s + max_d - operand_l.nbDims, operand_l.d,
              operand_l.nbDims * element_size);
  std::memcpy(r_s + max_d - operand_r.nbDims, operand_r.d,
              operand_r.nbDims * element_size);

  // set -1 for batch dimension, since batch size is not supposed to be
  // broadcasted
  if (operand_l_is_tensor) {
    if (max_d != l_d) {  // if broadcast beyond batch dimension, fail
      return false;
    }
    l_s[0] = -1;
  }
  if (operand_r_is_tensor) {
    if (max_d != r_d) {  // if broadcast beyond batch dimension, fail
      return false;
    }
    r_s[0] = -1;
  }

  // compare broadcast feasibility
  for (int i = max_d - 1; i >= 0; i--) {
    if ((l_s[i] != r_s[i]) && (l_s[i] != 1) && (r_s[i] != 1)) {
      return false;
    }
  }

  // output new TensorRT Dimension (stripping the batch dimension)
  operand_l_new_shape->nbDims = max_d - 1;
  std::memcpy(operand_l_new_shape->d, l_s + 1, (max_d - 1) * element_size);
  operand_r_new_shape->nbDims = max_d - 1;
  std::memcpy(operand_r_new_shape->d, r_s + 1, (max_d - 1) * element_size);

  return true;
}

inline bool DimsEqual(const nvinfer1::Dims& dim_l,
                      const nvinfer1::Dims& dim_r) {
  if (dim_l.nbDims != dim_r.nbDims) {
    return false;
  }
  for (int i = 0; i < dim_l.nbDims; i++) {
    if (dim_l.d[i] != dim_r.d[i]) {
      return false;
    }
  }
  return true;
}

inline nvinfer1::Dims GetTrtDimsForTensor(const tensorflow::Tensor& tensor) {
  nvinfer1::Dims dims;
  dims.nbDims = tensor.dims();
  for (int i = 0; i < dims.nbDims; i++) {
    dims.d[i] = tensor.dim_size(i);
  }
  return dims;
}

inline bool HasStaticShape(const nvinfer1::Dims& dims) {
  if (dims.nbDims < 0) return false;
  for (int d = 0; d < dims.nbDims; ++d) {
    if (dims.d[d] < 0) return false;
  }
  return true;
}

// Returns total number of elements in dims. Returning 0 means either some dim
// is 0 or the number of dims is 0.
// Note that for TF scalar constant, we always convert to dims [1].
int64_t TrtDimsNumElements(const nvinfer1::Dims& dims) {
  if (dims.nbDims == 0) return 0;
  int64_t count = 1;
  for (int d = 0; d < dims.nbDims; ++d) {
    count *= dims.d[d];
  }
  return count;
}

static std::vector<std::pair<int, int>> CreateSamePadding(
    const nvinfer1::DimsHW& stride, const nvinfer1::DimsHW& kernel,
    const std::vector<int64_t>& input_dims) {
  std::vector<std::pair<int, int>> padding(input_dims.size());
  CHECK_EQ(stride.nbDims, input_dims.size());  // TODO(jie): N+C? NC+?

  for (size_t i = 0; i < input_dims.size(); ++i) {
    // Formula to calculate the padding
    int p = ((input_dims[i] - 1) / stride.d[i]) * stride.d[i] + kernel.d[i] -
            input_dims[i];
    p = (p > 0) ? p : 0;

    // Right precedence padding, like in TensorFlow
    int left = p / 2;
    int right = p - left;

    VLOG(2) << "PADDING_" << i << " pre: " << left << ", post: " << right
            << "paras: " << input_dims[i] << ", " << stride.d[i] << ", "
            << "kernel: " << kernel.d[i];
    padding[i] = {left, right};
  }
  return padding;
}

string GetCommonNameScope(const string& op_name_a, const string& op_name_b) {
  size_t last_scope_separator = 0;
  const size_t min_size = std::min(op_name_a.size(), op_name_b.size());
  for (size_t i = 0; i < min_size; ++i) {
    if (op_name_a[i] != op_name_b[i]) break;
    if (op_name_a[i] == '/') last_scope_separator = i + 1;
  }
  return op_name_a.substr(0, last_scope_separator);
}

TRT_ShapedWeights::TRT_ShapedWeights(DataType type) : type_(type) {
  shape_.nbDims = 0;
}

TRT_ShapedWeights::TRT_ShapedWeights(DataType type, nvinfer1::Dims dims,
                                     Tensor tensor)
    : shape_(dims), type_(type), tensor_(tensor) {}

TRT_ShapedWeights::TRT_ShapedWeights(const TRT_ShapedWeights& rhs)
    : shape_(rhs.shape_), type_(rhs.type_), tensor_(rhs.tensor_) {}

int64_t TRT_ShapedWeights::count() const { return TrtDimsNumElements(shape_); }

nvinfer1::Weights TRT_ShapedWeights::GetTrtWeights() const {
  nvinfer1::DataType trt_type(nvinfer1::DataType::kFLOAT);
  TF_CHECK_OK(ConvertDType(type_, &trt_type));
  return nvinfer1::Weights{trt_type, GetValues(), count()};
}

size_t TRT_ShapedWeights::size_bytes() const {
  return this->count() * tensorflow::DataTypeSize(this->type_);
}

string TRT_ShapedWeights::DebugString() const {
  return StrCat("TRT_ShapedWeights(shape=", convert::DebugString(shape_),
                ", type=", type_,
                ", values=", reinterpret_cast<uintptr_t>(GetValues()), ")");
}

TRT_TensorOrWeights::TRT_TensorOrWeights(nvinfer1::ITensor* tensor,
                                         int batch_size)
    : tensor_(tensor),
      batch_size_(batch_size),
      weights_(DT_FLOAT),
      initialized_(true),
      is_tensor_(true) {}

TRT_TensorOrWeights::TRT_TensorOrWeights(const TRT_ShapedWeights& weights)
    : tensor_(nullptr),
      weights_(weights),
      initialized_(true),
      is_tensor_(false) {}

TRT_TensorOrWeights::TRT_TensorOrWeights(const TRT_TensorOrWeights& rhs)
    : tensor_(rhs.tensor_),
      batch_size_(rhs.batch_size_),
      weights_(rhs.weights_),
      initialized_(rhs.initialized_),
      is_tensor_(rhs.is_tensor_) {}

void TRT_TensorOrWeights::operator=(const TRT_TensorOrWeights& rhs) {
  tensor_ = rhs.tensor_;
  batch_size_ = rhs.batch_size_;
  weights_ = rhs.weights_;
  initialized_ = rhs.initialized_;
  is_tensor_ = rhs.is_tensor_;
}

nvinfer1::Dims TRT_TensorOrWeights::GetTrtDims() const {
  if (is_tensor()) {
    return tensor()->getDimensions();
  } else {
    return weights().shape_;
  }
}

string TRT_TensorOrWeights::DebugString() const {
  string output = "TRT_TensorOrWeights(type=";
  if (is_tensor()) {
    StrAppend(&output, "tensor @", reinterpret_cast<uintptr_t>(tensor_),
              ", shape=", convert::DebugString(tensor_->getDimensions()),
              ", batch_size=", batch_size_);
  } else {
    StrAppend(&output, "weights=", weights_.DebugString());
  }
  StrAppend(&output, ")");
  return output;
}

class TFAttrs {
 public:
  explicit TFAttrs(const tensorflow::NodeDef& tf_node) {
    for (const auto& attr : tf_node.attr()) {
      attrs_.insert({attr.first, &attr.second});
    }
  }

  bool count(const string& key) const { return attrs_.count(key); }

  tensorflow::AttrValue const* at(const string& key) const {
    if (!attrs_.count(key)) {
      LOG(FATAL) << "Attribute not found: " << key;
    }
    return attrs_.at(key);
  }

  template <typename T>
  T get(const string& key) const;

  template <typename T>
  T get(const string& key, const T& default_value) const {
    return attrs_.count(key) ? this->get<T>(key) : default_value;
  }

  std::vector<string> GetAllAttrKeys() const {
    std::vector<string> attr_list;
    for (const auto& attr_item : attrs_) {
      attr_list.emplace_back(attr_item.first);
    }
    return attr_list;
  }

 private:
  typedef std::map<string, tensorflow::AttrValue const*> AttrMap;
  AttrMap attrs_;
};

template <>
string TFAttrs::get<string>(const string& key) const {
  return this->at(key)->s();
}

template <>
std::vector<int> TFAttrs::get<std::vector<int>>(const string& key) const {
  auto attr = this->at(key)->list().i();
  return std::vector<int>(attr.begin(), attr.end());
}

template <>
std::vector<float> TFAttrs::get<std::vector<float>>(const string& key) const {
  auto attr = this->at(key)->list().f();
  return std::vector<float>(attr.begin(), attr.end());
}

template <>
nvinfer1::DataType TFAttrs::get<nvinfer1::DataType>(const string& key) const {
  nvinfer1::DataType trt_dtype(nvinfer1::DataType::kFLOAT);
  TF_CHECK_OK(ConvertDType(this->at(key)->type(), &trt_dtype));
  return trt_dtype;
}

template <>
tensorflow::DataType TFAttrs::get<tensorflow::DataType>(
    const string& key) const {
  return this->at(key)->type();
}

template <>
float TFAttrs::get<float>(const string& key) const {
  return this->at(key)->f();
}

template <>
bool TFAttrs::get<bool>(const string& key) const {
  return this->at(key)->b();
}

// TODO(jie): reorder4 & reorder2 should be merged?
// TODO(aaroey): fix the order of parameters.
template <typename T>
void Reorder4(const nvinfer1::DimsNCHW& shape, const T* idata,
              const nvinfer1::DimsNCHW& istrides, T* odata,
              const nvinfer1::DimsNCHW& ostrides) {
  for (int n = 0; n < shape.n(); ++n) {
    for (int c = 0; c < shape.c(); ++c) {
      for (int h = 0; h < shape.h(); ++h) {
        for (int w = 0; w < shape.w(); ++w) {
          odata[n * ostrides.n() + c * ostrides.c() + h * ostrides.h() +
                w * ostrides.w()] = idata[n * istrides.n() + c * istrides.c() +
                                          h * istrides.h() + w * istrides.w()];
        }
      }
    }
  }
}

template <typename T>
void Reorder2(const nvinfer1::DimsHW& shape, const T* idata,
              const nvinfer1::DimsHW& istrides, T* odata,
              const nvinfer1::DimsHW& ostrides) {
  for (int h = 0; h < shape.h(); ++h) {
    for (int w = 0; w < shape.w(); ++w) {
      odata[h * ostrides.h() + w * ostrides.w()] =
          idata[h * istrides.h() + w * istrides.w()];
    }
  }
}

// TODO(jie): fallback to tensorflow!!
void ReorderCKtoKC(const TRT_ShapedWeights& iweights,
                   TRT_ShapedWeights* oweights) {
  const int c = iweights.shape_.d[0];
  const int k = iweights.shape_.d[1];
  oweights->shape_.d[0] = k;
  oweights->shape_.d[1] = c;
  const nvinfer1::DimsHW istrides = {1, k};
  const nvinfer1::DimsHW ostrides = {c, 1};
  switch (iweights.type_) {
    case tensorflow::DataType::DT_FLOAT: {
      Reorder2({k, c}, static_cast<float const*>(iweights.GetValues()),
               istrides,
               // TODO(aaroey): get rid of all the const_cast like this.
               static_cast<float*>(const_cast<void*>(oweights->GetValues())),
               ostrides);
      break;
    }
    case tensorflow::DataType::DT_HALF: {
      Reorder2(
          {k, c}, static_cast<Eigen::half const*>(iweights.GetValues()),
          istrides,
          static_cast<Eigen::half*>(const_cast<void*>(oweights->GetValues())),
          ostrides);
      break;
    }
    default:
      LOG(FATAL) << "Unsupported type in reorder expected fp32 or fp16 but got "
                 << DataTypeString(iweights.type_);
  }
}

void ReorderRSCKToKCRS(const TRT_ShapedWeights& iweights,
                       TRT_ShapedWeights* oweights, const int num_groups) {
  CHECK_EQ(iweights.type_, oweights->type_);
  CHECK_EQ(iweights.size_bytes(), oweights->size_bytes());
  // K indexes over output channels, C over input channels, and R and S over the
  // height and width of the convolution
  const int r = iweights.shape_.d[0];
  const int s = iweights.shape_.d[1];
  // TRT requires GKcRS, while TF depthwise has RSCK where c=1, C=G
  const int c = iweights.shape_.d[2] / num_groups;
  const int k = iweights.shape_.d[3] * num_groups;
  VLOG(2) << "num_groups: " << num_groups
          << "c" << iweights.shape_.d[2] << " then " << c
          << "k" << iweights.shape_.d[3] << " then " << k
          << "r" << iweights.shape_.d[0] << " then " << r
          << "s" << iweights.shape_.d[1] << " then " << s;
  oweights->shape_.d[0] = k / num_groups;
  oweights->shape_.d[1] = c * num_groups;
  oweights->shape_.d[2] = r;
  oweights->shape_.d[3] = s;
  const nvinfer1::DimsNCHW istrides = {1, k, s * k * c, c * k};
  const nvinfer1::DimsNCHW ostrides = {c * r * s, r * s, s, 1};
  switch (iweights.type_) {
    case tensorflow::DataType::DT_FLOAT: {
      Reorder4({k, c, r, s}, static_cast<float const*>(iweights.GetValues()),
               istrides,
               static_cast<float*>(const_cast<void*>(oweights->GetValues())),
               ostrides);
      break;
    }
    case tensorflow::DataType::DT_HALF: {
      Reorder4(
          {k, c, r, s}, static_cast<Eigen::half const*>(iweights.GetValues()),
          istrides,
          static_cast<Eigen::half*>(const_cast<void*>(oweights->GetValues())),
          ostrides);
      break;
    }

    default:
      LOG(FATAL) << "Unsupported type, expected fp32 or fp16 but got "
                 << DataTypeString(iweights.type_);
  }
}

TRT_ShapedWeights TrtWeightStore::GetTempWeights(tensorflow::DataType type,
                                                 const nvinfer1::Dims& dims) {
  TensorShape shape;
  // TODO(laigd): make it return a status.
  TF_CHECK_OK(TensorShapeUtils::MakeShape(dims.d, dims.nbDims, &shape));
  // TODO(jie): check weights size_bytes. 0 means type error
  Tensor tensor(type, shape);
  TRT_ShapedWeights weights(type, dims, tensor);
  store_.emplace_back(std::move(tensor));
  return weights;
}

TrtNodeValidator::TrtNodeValidator() { RegisterOpValidators(); }

Status TrtNodeValidator::ValidateNode(
    const tensorflow::NodeDef& node_def,
    const std::vector<TRT_TensorOrWeights>& inputs) {
  const auto iter = op_validators_.find(node_def.op());
  if (iter == op_validators_.end()) {
    // If validator is not registered, it means no validation is needed.
    return Status::OK();
  }

  OpConverter validator = iter->second;
  OpConverterParams params(
      /*arg_converter=*/nullptr, node_def, inputs, /*arg_outputs=*/nullptr,
      /*arg_validation_only=*/true, &weight_store_);
  Status status = validator(&params);
  return status;
}

Converter::Converter(nvinfer1::INetworkDefinition* trt_network, bool is_fp16)
    : trt_network_(trt_network), is_fp16_(is_fp16) {
  this->RegisterOpConverters();
}

Status Converter::ConvertNode(const NodeDef& node_def) {
  std::vector<TRT_TensorOrWeights> inputs, outputs;
  TF_RETURN_IF_ERROR(this->GetInputs(node_def, &inputs));

  OpConverterParams params(this, node_def, inputs, &outputs,
                           /*arg_validation_only=*/false, &weight_store_);
  const string& op = node_def.op();
  if (PluginFactoryTensorRT::GetInstance()->IsPlugin(op)) {
    TF_RETURN_IF_ERROR(plugin_converter_(&params));
  } else {
    if (!op_registry_.count(op)) {
      return errors::Unimplemented("No converter registered for op: " + op);
    }
    OpConverter op_converter = op_registry_.at(op);
    TF_RETURN_IF_ERROR(op_converter(&params));
  }

  for (size_t i = 0; i < outputs.size(); ++i) {
    TRT_TensorOrWeights& output = outputs[i];
    string output_name = node_def.name();
    if (i != 0) output_name = StrCat(output_name, ":", i);
    // We need to check the name before setting it. For Identity op where the
    // output is the input, if its input is one of the engine input, setting
    // the name here will overwrite engine input bindings which will cause
    // runtime error.
    if (output.is_tensor()) {
      const char* tensor_name = output.tensor()->getName();
      if (tensor_name == nullptr || std::strlen(tensor_name) == 0) {
        output.tensor()->setName(output_name.c_str());
      }
    }
    VLOG(2) << "Adding out tensor " << output_name << ": "
            << output.DebugString();
    Status status = AddTensorOrWeights(output_name, output);
    if (!status.ok()) {
      return Status(status.code(),
                    StrCat("Failed to add output for node ", node_def.name(),
                           ": ", status.error_message()));
    }
  }
  return Status::OK();
}

Status Converter::AddInputTensor(const string& name, nvinfer1::DataType dtype,
                                 const nvinfer1::Dims& dims, int batch_size) {
  // We verify the batch size only for the input nodes, and rely on individual
  // op converter to ensure the batch size of the outputs is not changed.
  // TODO(laigd): we need to test this properties.
  Status status = MaybeUpdateBatchSize(batch_size);
  if (!status.ok()) {
    return Status(status.code(), StrCat("Batch size doesn't match for tensor ",
                                        name, ": ", status.error_message()));
  }
  nvinfer1::ITensor* tensor = network()->addInput(name.c_str(), dtype, dims);
  if (tensor == nullptr) {
    return errors::InvalidArgument("Failed to create Input layer tensor ", name,
                                   " rank=", dims.nbDims);
  }
  status = AddTensorOrWeights(name, TRT_TensorOrWeights(tensor));
  if (!status.ok()) {
    return Status(status.code(), StrCat("Failed to add input tensor ", name,
                                        ": ", status.error_message()));
  }
  return Status::OK();
}

Status Converter::RenameAndMarkOutputTensors(
    const std::vector<std::pair<string, string>>& output_tensors) {
  for (const auto& output : output_tensors) {
    TRT_TensorOrWeights tensor_or_weights;
    TF_RETURN_IF_ERROR(GetTensorOrWeights(output.first, &tensor_or_weights));
    if (!tensor_or_weights.is_tensor()) {
      return errors::InvalidArgument("Output ", output.first,
                                     " is weights not tensor");
    }
    nvinfer1::ITensor* tensor = tensor_or_weights.tensor();
    if (tensor == nullptr) {
      return errors::NotFound("Output tensor not found: ", output.first);
    }
    tensor->setName(output.second.c_str());
    VLOG(1) << "Marking output tensor " << output.first << ", as output tensor "
            << output.second;
    network()->markOutput(*tensor);
  }
  return Status::OK();
}

Status Converter::MaybeUpdateBatchSize(int batch_size) {
  // OK iff either is unknown or they equal to each other.
  if (this->batch_size_ < 0 || batch_size < 0 ||
      this->batch_size_ == batch_size) {
    if (this->batch_size_ < 0 && batch_size >= 0) {
      this->batch_size_ = batch_size;
    }
    return Status::OK();
  }
  return errors::InvalidArgument(
      "Provided batch size does not match converter batch size: ", batch_size,
      " vs ", batch_size_);
}

Status Converter::AddTensorOrWeights(const string& name,
                                     TRT_TensorOrWeights input) {
  // Set the batch size of the tensor, using batch size collected from the
  // input tensors to the TRT subgraph at the beginning of the conversion.
  // We rely on the individual op converter to understand the semantics of the
  // TF node, and make sure it doesn't change the batch size nor introduce
  // intra-element dependency inside the batch.
  if (input.is_tensor()) input.set_batch_size(batch_size_);
  if (trt_tensors_.insert({name, std::move(input)}).second) return Status::OK();
  return errors::AlreadyExists("tensor/weights ", name, " already exist.");
}

Status Converter::GetTensorOrWeights(const string& name,
                                     TRT_TensorOrWeights* output) {
  if (!trt_tensors_.count(name)) {
    return errors::NotFound("Tensor or weights with name ", name,
                            " could not be found.");
  }
  *output = trt_tensors_.at(name);
  return Status::OK();
}

Status Converter::TransposeTensor(nvinfer1::ITensor* input_tensor,
                                  const std::vector<int>& order_with_batch_dim,
                                  const nvinfer1::ITensor** output_tensor) {
  const auto dims = input_tensor->getDimensions();

  if (order_with_batch_dim.size() - 1 != size_t(dims.nbDims)) {
    return tensorflow::errors::InvalidArgument(
        "Rank of perm for transpose does not match with that of the input.");
  }
  if (order_with_batch_dim[0] != 0) {
    return tensorflow::errors::Unimplemented(
        "Transpose at batch dimension is not supported.");
  }

  nvinfer1::IShuffleLayer* layer = this->network()->addShuffle(*input_tensor);
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, "TF-TRT Internal Transpose");

  nvinfer1::Permutation permutation;
  for (int32_t i = 0; i < dims.nbDims; ++i) {
    permutation.order[i] = order_with_batch_dim[i + 1] - 1;
  }
  layer->setFirstTranspose(permutation);

  nvinfer1::Dims reshape_dims;
  reshape_dims.nbDims = dims.nbDims;
  for (int32_t i = 0; i < reshape_dims.nbDims; ++i) {
    reshape_dims.d[i] = 0;
    // TODO(aaroey): why not transposing the types as well?
    reshape_dims.type[i] = dims.type[i];
  }
  layer->setReshapeDimensions(reshape_dims);

  *output_tensor = layer->getOutput(0);
  return tensorflow::Status::OK();
}

Status Converter::PrepareTensorForShape(const TRT_TensorOrWeights& input,
                                        const nvinfer1::Dims& dims,
                                        const nvinfer1::ITensor** tensor) {
  // If -1 is not used for one of the dims, we can check if the shapes are
  // compatible.
  bool can_check_shapes = true;
  for (int i = 0; i < dims.nbDims; i++) {
    if (dims.d[i] == -1) {
      can_check_shapes = false;
      break;
    }
  }
  if (can_check_shapes &&
      TrtDimsNumElements(input.GetTrtDims()) != TrtDimsNumElements(dims)) {
    return tensorflow::errors::InvalidArgument(
        "Reshape shapes are not compatible.");
  }

  if (input.is_tensor()) {
    if (DimsEqual(input.GetTrtDims(), dims)) {
      *tensor = input.tensor();
    } else {
      nvinfer1::IShuffleLayer* layer = this->network()->addShuffle(
          *const_cast<nvinfer1::ITensor*>(input.tensor()));
      TFTRT_RETURN_ERROR_IF_NULLPTR(layer, "TF-TRT Internal Reshape");
      layer->setReshapeDimensions(dims);
      *tensor = layer->getOutput(0);
    }
  } else {
    nvinfer1::IConstantLayer* layer =
        this->network()->addConstant(dims, input.weights().GetTrtWeights());
    TFTRT_RETURN_ERROR_IF_NULLPTR(layer, "TF-TRT Internal Reshape");
    *tensor = layer->getOutput(0);
  }
  return tensorflow::Status::OK();
}

Status Converter::GetInputs(const tensorflow::NodeDef& node_def,
                            std::vector<TRT_TensorOrWeights>* inputs) const {
  for (auto const& input_name : node_def.input()) {
    /*************************************************************************
     * TODO(jie): handle case 1) here.
     * Normalizes the inputs and extracts associated metadata:
     * 1) Inputs can contain a colon followed by a suffix of characters.
     *    That suffix may be a single number (e.g. inputName:1) or several
     *    word characters separated from a number by a colon
     *    (e.g. inputName:foo:1). The
     *    latter case is used to denote inputs and outputs of functions.
     * 2) Control dependency inputs contain caret at the beginning and we
     *    remove this and annotate the edge as a control dependency.
     ************************************************************************/
    // skip control nodes
    if (input_name[0] == '^') continue;
    string name = input_name;
    auto last = name.find_last_of(':');
    // TODO(aaroey): use TensorId
    if (last != string::npos && last + 2 == name.size() &&
        name[last + 1] == '0') {
      name.erase(last);
    }

    if (trt_tensors_.count(name)) {
      TRT_TensorOrWeights input = trt_tensors_.at(name);
      inputs->push_back(input);
      VLOG(2) << "Retrieved input " << name << ": " << input.DebugString();
    } else {
      // TODO(aaroey): this should not happen, make it a CHECK.
      // TODO(aaroey): use StrCat for pattern like this.
      string msg("Node ");
      StrAppend(&msg, node_def.name(), " should have an input named '", name,
                "' but it is not available");
      LOG(ERROR) << msg;
      return tensorflow::errors::InvalidArgument(msg);
    }
  }
  return tensorflow::Status::OK();
}

TRT_ShapedWeights ConvertFP32ToFP16(TrtWeightStore* store,
                                    const TRT_ShapedWeights& weights_src) {
  auto dtype_new = tensorflow::DataType::DT_HALF;
  TRT_ShapedWeights weights =
      store->GetTempWeights(dtype_new, weights_src.shape_);
  const float* src = static_cast<const float*>(weights_src.GetValues());
  Eigen::half* dst = const_cast<Eigen::half*>(
      static_cast<Eigen::half const*>(weights.GetValues()));
  for (int64_t i = 0; i < weights_src.count(); i++) {
    dst[i] = Eigen::half_impl::float_to_half_rtne(src[i]);
  }
  return weights;
}

// ****************************************************************************
// Constant folding functions
// TODO(jie): once optimizer kicks in, we should have done constant folding
// there.
// *****************************************************************************
struct LambdaFactory {
  enum class OP_CATEGORY : int { RSQRT = 0, NEG, ADD, MUL, SUB, RECIP };
  OP_CATEGORY op;

  template <typename T>
  std::function<T(T)> unary() {
    switch (op) {
      case OP_CATEGORY::RSQRT: {
        VLOG(2) << "RSQRT GETS DONE";
        return [](T t) -> T { return 1.0 / sqrt(t); };
      }
      case OP_CATEGORY::NEG:
        return [](T t) -> T { return -t; };
      case OP_CATEGORY::RECIP:
        return [](T t) -> T { return 1.0 / t; };
      default:
        VLOG(2) << "Not supported op for unary: " << static_cast<int>(op);
        return nullptr;
    }
  }

  template <typename T>
  std::function<T(T, T)> binary() {
    switch (op) {
      case OP_CATEGORY::ADD:
        return [](T l, T r) -> T { return l + r; };
      case OP_CATEGORY::SUB:
        return [](T l, T r) -> T { return l - r; };
      case OP_CATEGORY::MUL:
        return [](T l, T r) -> T { return l * r; };
      default:
        LOG(WARNING) << "Not supported op for binary: " << static_cast<int>(op);
    }
    return [](T l, T r) -> T {
      LOG(FATAL) << "Unsupported op type ";
      return l;
    };
  }

  template <typename T>
  std::function<T(T)> broadcast_r(T val) {
    VLOG(2) << "LAMBDA VAL : " << val;
    switch (op) {
      case OP_CATEGORY::ADD:
        return [val](T l) -> T {
          VLOG(2) << "LAMBDA VAL : " << val;
          return l + val;
        };
      case OP_CATEGORY::SUB:
        return [val](T l) -> T {
          VLOG(2) << "LAMBDA VAL : " << val;
          return l - val;
        };
      case OP_CATEGORY::MUL:
        return [val](T l) -> T {
          VLOG(2) << "LAMBDA VAL : " << val;
          return l * val;
        };
      default:
        LOG(WARNING) << "Not supported op for binary: " << static_cast<int>(op);
    }
    return [val](T l) -> T {
      LOG(FATAL) << "Unsupported op type ";
      return l;
    };
  }

  template <typename T>
  std::function<T(T)> broadcast_l(T val) {
    VLOG(2) << "LAMBDA VAL : " << val;
    switch (op) {
      case OP_CATEGORY::ADD:
        return [val](T l) -> T {
          VLOG(2) << "LAMBDA VAL : " << val;
          return val + l;
        };
      case OP_CATEGORY::SUB:
        return [val](T l) -> T {
          VLOG(2) << "LAMBDA VAL : " << val;
          return val - l;
        };
      case OP_CATEGORY::MUL:
        return [val](T l) -> T {
          VLOG(2) << "LAMBDA VAL : " << val;
          return val * l;
        };
      default:
        LOG(ERROR) << "Not supported op for binary: " << static_cast<int>(op);
    }
    return [val](T l) -> T {
      LOG(FATAL) << "Unsupported op type ";
      return l;
    };
  }
};

template <>
std::function<Eigen::half(Eigen::half)> LambdaFactory::unary<Eigen::half>() {
  switch (op) {
    case OP_CATEGORY::RSQRT: {
      VLOG(2) << "RSQRT GETS DONE";
      return [](Eigen::half t) -> Eigen::half {
        return Eigen::half(1.0 / sqrt(static_cast<float>(t)));
      };
    }
    case OP_CATEGORY::NEG:
      return [](Eigen::half t) -> Eigen::half { return -t; };
    // TODO(aaroey): can we support RECIP?
    default:
      VLOG(2) << "Not supported op for unary: " << static_cast<int>(op);
      return nullptr;
  }
}

tensorflow::Status UnaryCompute(const TRT_ShapedWeights& iweights,
                                TRT_ShapedWeights* oweights,
                                LambdaFactory unary_op) {
  CHECK_EQ(iweights.type_, oweights->type_);
  switch (iweights.type_) {
    case tensorflow::DataType::DT_FLOAT: {
      auto inp = static_cast<float const*>(iweights.GetValues());
      auto oup = static_cast<float*>(const_cast<void*>(oweights->GetValues()));
      std::transform(inp, inp + iweights.count(), oup, unary_op.unary<float>());
      break;
    }
    case tensorflow::DataType::DT_HALF: {
      auto inp = static_cast<Eigen::half const*>(iweights.GetValues());
      auto oup =
          static_cast<Eigen::half*>(const_cast<void*>(oweights->GetValues()));
      std::transform(inp, inp + iweights.count(), oup,
                     unary_op.unary<Eigen::half>());
      break;
    }
    default:
      return tensorflow::errors::Unimplemented(
          "Data type not supported: " +
          tensorflow::DataTypeString(iweights.type_));
  }
  return tensorflow::Status::OK();
}

// TODO(jie): broadcast is needed yet not implemented.
// Only implemented channel wise for the time being
tensorflow::Status BinaryTensorOpWeight(OpConverterParams* params,
                                        const nvinfer1::ITensor* tensor,
                                        TRT_ShapedWeights weights,
                                        bool swapped_inputs) {
  const auto& node_def = params->node_def;
  // tensor is the left operand while weights is the right operand;
  // when swapped_inputs set to true, those two are swapped.
  // TODO(aaroey): use a set.
  if (node_def.op() != "Sub" && node_def.op() != "Add" &&
      node_def.op() != "Mul" && node_def.op() != "Div" &&
      node_def.op() != "RealDiv") {
    return tensorflow::errors::Unimplemented(
        "op not supported: " + node_def.op() + ", at: " + node_def.name());
  }

  // Check type consistency
  nvinfer1::DataType ttype;
  TF_RETURN_IF_ERROR(ConvertDType(weights.type_, &ttype));

  // Check scale mode
  auto dims_w = weights.shape_;
  auto dims_t = tensor->getDimensions();

  // TODO(jie): addScale checks for input tensor dimension
  if (dims_t.nbDims != 3) {
    return tensorflow::errors::InvalidArgument(
        "addScale requires tensor with rank 3, " + node_def.name());
  }

  // default to element-wise
  auto scale_mode = nvinfer1::ScaleMode::kELEMENTWISE;

  // TODO(jie): maybe use a permutation instead to support more cases;
  bool permutation_flag = false;

  if (weights.count() == 1) {
    VLOG(2) << "UNIFORM";
    scale_mode = nvinfer1::ScaleMode::kUNIFORM;
  } else {
    // no broadcasting on Batch dimension;
    VLOG(2) << "WEIGHTS DIM: " << dims_w.nbDims
            << " tensor DIM: " << dims_t.nbDims;
    if (dims_w.nbDims == dims_t.nbDims + 1) {
      if (dims_w.d[0] == 1) {
        for (int i = 1; i < dims_w.nbDims; i++) {
          dims_w.d[i - 1] = dims_w.d[i];
        }
        dims_w.nbDims--;
      } else {
        return tensorflow::errors::InvalidArgument(
            "Binary op cannot operate on batch, " + node_def.name());
      }
    }

    if (dims_w.nbDims == dims_t.nbDims && dims_w.d[0] == dims_t.d[0]) {
      scale_mode = nvinfer1::ScaleMode::kELEMENTWISE;
      // default is element;
      for (int i = 1; i < dims_w.nbDims; i++) {
        if (dims_w.d[i] != dims_t.d[i]) {
          // if dimension does not match, switch back to channel;
          VLOG(2) << "channel";
          scale_mode = nvinfer1::ScaleMode::kCHANNEL;
          break;
        }
      }
      // if channel as candidate, validate it
      if (scale_mode == nvinfer1::ScaleMode::kCHANNEL) {
        for (int i = 1; i < dims_w.nbDims; i++) {
          if (dims_w.d[i] != 1)
            return tensorflow::errors::InvalidArgument(
                "Weight shape not compatible at, " + node_def.name());
        }
      } else {
        VLOG(2) << "elementwise";
      }
    } else if (dims_w.nbDims == 1 &&
               dims_w.d[0] == dims_t.d[dims_t.nbDims - 1]) {
      // channel wise and broadcast required;
      permutation_flag = true;
      scale_mode = nvinfer1::ScaleMode::kCHANNEL;
    } else {
      return tensorflow::errors::InvalidArgument(
          "Weight shape not compatible at, " + node_def.name());
    }
  }

  // transpose last dimension
  std::vector<int> permutation(dims_t.nbDims + 1);
  if (permutation_flag) {
    if (scale_mode == nvinfer1::ScaleMode::kCHANNEL && dims_t.nbDims > 1) {
      // we swap the last dimension into channel for trt.
      // because of tensorflow default broadcasting rules.
      for (int i = 0; i < static_cast<int>(permutation.size()); i++) {
        permutation[i] = i;
      }
      permutation[1] = dims_t.nbDims;
      permutation[dims_t.nbDims] = 1;
      TF_RETURN_IF_ERROR(params->converter->TransposeTensor(
          const_cast<nvinfer1::ITensor*>(tensor), permutation, &tensor));
    } else {
      return tensorflow::errors::InvalidArgument(
          "Transpose cannot be applied, " + node_def.name());
    }
  }

  if (params->converter->is_fp16()) {
    weights = ConvertFP32ToFP16(params->weight_store, weights);
  }

  // prepare weights
  TRT_ShapedWeights shift_weights(weights.type_);
  TRT_ShapedWeights scale_weights(weights.type_);
  TRT_ShapedWeights power_weights(weights.type_);

  // Maybe I should do a switch
  if (node_def.op() == "Sub") {
    if (swapped_inputs) {
      shift_weights = weights;
      nvinfer1::IUnaryLayer* layer = params->converter->network()->addUnary(
          *const_cast<nvinfer1::ITensor*>(tensor),
          nvinfer1::UnaryOperation::kNEG);
      TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());
      tensor = layer->getOutput(0);
    } else {
      TRT_ShapedWeights neg_weights =
          params->weight_store->GetTempWeights(weights);
      LambdaFactory unary_op;
      unary_op.op = LambdaFactory::OP_CATEGORY::NEG;
      TF_RETURN_IF_ERROR(UnaryCompute(weights, &neg_weights, unary_op));
      shift_weights = neg_weights;
    }
  } else if (node_def.op() == "Div" || node_def.op() == "RealDiv") {
    if (swapped_inputs) {
      scale_weights = weights;
      nvinfer1::IUnaryLayer* layer = params->converter->network()->addUnary(
          *const_cast<nvinfer1::ITensor*>(tensor),
          nvinfer1::UnaryOperation::kRECIP);
      TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());
      tensor = layer->getOutput(0);
    } else {
      TRT_ShapedWeights recip_weights =
          params->weight_store->GetTempWeights(weights);
      LambdaFactory unary_op;
      unary_op.op = LambdaFactory::OP_CATEGORY::RECIP;
      TF_RETURN_IF_ERROR(UnaryCompute(weights, &recip_weights, unary_op));
      scale_weights = recip_weights;
    }
  } else if (node_def.op() == "Mul") {
    scale_weights = weights;
  } else if (node_def.op() == "Add") {
    shift_weights = weights;
  } else {
    return tensorflow::errors::Unimplemented("Binary op not supported: " +
                                             node_def.op());
  }

  nvinfer1::IScaleLayer* layer = params->converter->network()->addScale(
      *const_cast<nvinfer1::ITensor*>(tensor), scale_mode,
      shift_weights.GetTrtWeights(), scale_weights.GetTrtWeights(),
      power_weights.GetTrtWeights());
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());

  const nvinfer1::ITensor* output_tensor = layer->getOutput(0);
  // transpose back dimension
  if (permutation_flag) {
    TF_RETURN_IF_ERROR(params->converter->TransposeTensor(
        const_cast<nvinfer1::ITensor*>(output_tensor), permutation,
        &output_tensor));
  }

  // Pass the output
  params->outputs->push_back(
      TRT_TensorOrWeights(const_cast<nvinfer1::ITensor*>(output_tensor)));
  return tensorflow::Status::OK();
}

enum class ConvolutionType { DEFAULT, DEPTHWISE_CONV };

tensorflow::Status ConvertConv2DHelper(OpConverterParams* params, int group) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  const nvinfer1::ITensor* tensor = inputs.at(0).tensor();
  TFAttrs attrs(node_def);

  int h_index = 2;
  int w_index = 3;
  auto data_format = attrs.get<string>("data_format");
  if (data_format == "NHWC") {
    TF_RETURN_IF_ERROR(params->converter->TransposeTensor(
        const_cast<nvinfer1::ITensor*>(tensor), {0, 3, 1, 2}, &tensor));
    h_index = 1;
    w_index = 2;
    // TODO(jie): transpose it
  }

  // tensor after transpose (NCHW)
  const auto tensor_dim = tensor->getDimensions();

  int num_groups = group;
  if (num_groups == 0) num_groups = tensor_dim.d[0];  // depthwise convolution
  VLOG(2) << "groups count: " << num_groups;

  TRT_ShapedWeights weights_rsck = inputs.at(1).weights();
  VLOG(2) << "weight shape: " << weights_rsck.DebugString();
  if (weights_rsck.shape_.nbDims != 4) {
    return tensorflow::errors::Internal(
        "Conv2D expects kernel of dimension 4, at: " + node_def.name());
  }
  if (params->converter->is_fp16()) {
    weights_rsck =
        ConvertFP32ToFP16(params->weight_store, inputs.at(1).weights());
  }

  TRT_ShapedWeights weights =
      params->weight_store->GetTempWeights(weights_rsck);
  ReorderRSCKToKCRS(weights_rsck, &weights, num_groups);
  TRT_ShapedWeights biases(weights.type_);
  const int noutput = weights.shape_.d[0] * num_groups;
  nvinfer1::DimsHW kernel_size;
  kernel_size.h() = weights.shape_.d[2];
  kernel_size.w() = weights.shape_.d[3];
  VLOG(2) << "RSCK: " << weights.DebugString();
  VLOG(2) << "kernel size: " << kernel_size.h() << ", " << kernel_size.w();

  // TODO(jie): stride. (NHWC/NCHW)
  const auto tf_stride = attrs.get<std::vector<int>>("strides");
  VLOG(2) << "h_INDEX" << h_index << ", w_index " << w_index;
  VLOG(2) << "stride: " << tf_stride[0] << tf_stride[1] << tf_stride[2]
          << tf_stride[3];
  const nvinfer1::DimsHW stride(tf_stride[h_index], tf_stride[w_index]);

  std::vector<std::pair<int, int>> padding;
  // TODO(jie): padding.
  if (attrs.get<string>("padding") == "SAME") {
    // This is NCHW tensor with no batch dimension.
    //  1 -> h
    //  2 -> w
    padding = CreateSamePadding(
        stride, kernel_size,
        {static_cast<int>(tensor_dim.d[1]), static_cast<int>(tensor_dim.d[2])});
  } else {
    padding = {{0, 0}, {0, 0}};
  }

  if (padding[0].first != padding[0].second ||
      padding[1].first != padding[1].second) {
    // TODO(jie): handle asymmetric padding
    VLOG(2) << "Padding!!!: " << padding[0].first << padding[0].second
            << padding[1].first << padding[1].second;
    VLOG(2) << "TENSOR before: " << DebugString(tensor->getDimensions());
    auto pad_layer = params->converter->network()->addPadding(
        *const_cast<nvinfer1::ITensor*>(tensor),
        nvinfer1::DimsHW(padding[0].first, padding[1].first),
        nvinfer1::DimsHW(padding[0].second, padding[1].second));
    TFTRT_RETURN_ERROR_IF_NULLPTR(pad_layer, node_def.name());
    padding = {{0, 0}, {0, 0}};
    tensor = pad_layer->getOutput(0);
    VLOG(2) << "TENSOR after: " << DebugString(tensor->getDimensions());
  }

  nvinfer1::IConvolutionLayer* layer =
      params->converter->network()->addConvolution(
          *const_cast<nvinfer1::ITensor*>(tensor), noutput, kernel_size,
          weights.GetTrtWeights(), biases.GetTrtWeights());
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());

  layer->setStride(stride);
  layer->setPadding({padding[0].first, padding[1].first});
  layer->setName(node_def.name().c_str());
  layer->setNbGroups(num_groups);
  const nvinfer1::ITensor* output_tensor = layer->getOutput(0);
  VLOG(2) << "TENSOR out: " << DebugString(output_tensor->getDimensions());
  VLOG(2) << "data_format: " << data_format;
  if (data_format == "NHWC") {
    // TODO(jie): transpose it back!
    TF_RETURN_IF_ERROR(params->converter->TransposeTensor(
        const_cast<nvinfer1::ITensor*>(output_tensor), {0, 2, 3, 1},
        &output_tensor));
  }
  params->outputs->push_back(
      TRT_TensorOrWeights(const_cast<nvinfer1::ITensor*>(output_tensor)));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertConv2DHelper(OpConverterParams* params,
                                       ConvolutionType type) {
  switch (type) {
    case ConvolutionType::DEFAULT:
      return ConvertConv2DHelper(params, 1);
    case ConvolutionType::DEPTHWISE_CONV:
      return ConvertConv2DHelper(params, 0);
  }
  return tensorflow::errors::Unimplemented("unsupported convolution type at, " +
                                           params->node_def.name());
}

tensorflow::Status BinaryTensorOpTensor(OpConverterParams* params,
                                        const TRT_TensorOrWeights& operand_l,
                                        const TRT_TensorOrWeights& operand_r) {
  const auto& node_def = params->node_def;
  static const std::unordered_map<string, nvinfer1::ElementWiseOperation> ops{
      {"Add", nvinfer1::ElementWiseOperation::kSUM},
      {"Mul", nvinfer1::ElementWiseOperation::kPROD},
      {"Sub", nvinfer1::ElementWiseOperation::kSUB},
      {"Div", nvinfer1::ElementWiseOperation::kDIV},
      {"RealDiv", nvinfer1::ElementWiseOperation::kDIV},
      {"Minimum", nvinfer1::ElementWiseOperation::kMIN},
      {"Maximum", nvinfer1::ElementWiseOperation::kMAX},
  };

  const nvinfer1::ITensor* tensor_l;
  const nvinfer1::ITensor* tensor_r;

  nvinfer1::Dims dim_l;
  nvinfer1::Dims dim_r;

  if (!TensorRTGetBroadcastShape(operand_l.GetTrtDims(), operand_l.is_tensor(),
                                 operand_r.GetTrtDims(), operand_r.is_tensor(),
                                 &dim_l, &dim_r)) {
    return tensorflow::errors::InvalidArgument(
        "Binary op broadcast scheme not supported by TensorRT op: " +
        node_def.op() + ", at: " + node_def.name());
  }

  TF_RETURN_IF_ERROR(
      params->converter->PrepareTensorForShape(operand_l, dim_l, &tensor_l));
  TF_RETURN_IF_ERROR(
      params->converter->PrepareTensorForShape(operand_r, dim_r, &tensor_r));

  // get trt type & shape
  TFAttrs attrs(node_def);
  // maybe this part has to be moved into the block of rsqrt later
  nvinfer1::DataType dtype = attrs.get<nvinfer1::DataType>("T");

  // check type consistency
  TFTRT_CHECK_EQ_TYPE(tensor_l->getType(), dtype);
  TFTRT_CHECK_EQ_TYPE(tensor_r->getType(), dtype);
  auto op_pair = ops.find(node_def.op());
  if (op_pair == ops.end()) {
    return tensorflow::errors::Unimplemented(
        "binary op: ", node_def.op(), " not supported at: ", node_def.name());
  }

  nvinfer1::IElementWiseLayer* layer =
      params->converter->network()->addElementWise(
          // TODO(aaroey): will tensor_l/tensor_r get modified?
          *const_cast<nvinfer1::ITensor*>(tensor_l),
          *const_cast<nvinfer1::ITensor*>(tensor_r), op_pair->second);
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());

  nvinfer1::ITensor* output_tensor = layer->getOutput(0);

  // pass the output
  params->outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertPlugin(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  // prepare input
  std::vector<nvinfer1::ITensor*> all_inputs;
  for (auto input : inputs) {
    all_inputs.emplace_back(const_cast<nvinfer1::ITensor*>(input.tensor()));
  }

  // plugin is owned by PluginFactory
  // TODO(jie): destroy plugins later (resource management)
  PluginTensorRT* plugin =
      PluginFactoryTensorRT::GetInstance()->CreatePlugin(node_def.op());

  // passing attributes
  // TODO(jie): support more general attribute
  TFAttrs attrs(node_def);
  auto attr_key_vector = attrs.GetAllAttrKeys();
  for (auto attr_key : attr_key_vector) {
    // TODO(jie): support only list of float for toy example here.
    auto data = attrs.get<std::vector<float>>(attr_key);
    size_t size_data = data.size() * sizeof(float);
    if (!plugin->SetAttribute(attr_key, static_cast<void*>(data.data()),
                              size_data)) {
      return tensorflow::errors::InvalidArgument("plugin SetAttribute failed");
    }
  }

  nvinfer1::IPluginLayer* layer = params->converter->network()->addPlugin(
      &all_inputs[0], static_cast<int>(inputs.size()), *plugin);

  for (int i = 0; i < layer->getNbOutputs(); i++) {
    nvinfer1::ITensor* output_tensor = layer->getOutput(i);
    params->outputs->push_back(TRT_TensorOrWeights(output_tensor));
  }
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertTranspose(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  if (inputs.size() != 2 || !inputs.at(0).is_tensor() ||
      !inputs.at(1).is_weights()) {
    return tensorflow::errors::InvalidArgument(
        "Input expects tensor and weights, at ", params->node_def.name());
  }

  // Get the permutation from weights.
  TRT_ShapedWeights weights = inputs.at(1).weights();
  const int* weights_ptr =
      static_cast<int*>(const_cast<void*>(weights.GetValues()));
  std::vector<int> perm(weights_ptr, weights_ptr + weights.count());

  // Verify the permutation.
  nvinfer1::ITensor* input_tensor =
      const_cast<nvinfer1::ITensor*>(inputs.at(0).tensor());
  if (perm.size() - 1 != size_t(input_tensor->getDimensions().nbDims)) {
    return errors::InvalidArgument(
        "Rank of perm for transpose does not match with that of the input.");
  }
  if (perm[0] != 0) {
    return errors::Unimplemented(
        "Transpose at batch dimension is not supported.");
  }

  if (params->validation_only) return Status::OK();

  // Start conversion.
  const nvinfer1::ITensor* output_tensor = nullptr;
  TF_RETURN_IF_ERROR(
      params->converter->TransposeTensor(input_tensor, perm, &output_tensor));
  params->outputs->push_back(
      TRT_TensorOrWeights(const_cast<nvinfer1::ITensor*>(output_tensor)));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertReshape(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  if (inputs.size() != 2 || !inputs.at(1).is_weights()) {
    return tensorflow::errors::InvalidArgument(
        "Input expects weights for shape, at ", node_def.name());
  }

  TRT_TensorOrWeights input_tensor = inputs.at(0);
  TRT_ShapedWeights weights = inputs.at(1).weights();
  if (weights.count() == 0) {
    return tensorflow::errors::Unimplemented(
        "Reshape to shape=[] is not supported, at ", node_def.name());
  }

  const int* weights_ptr =
      static_cast<int*>(const_cast<void*>(weights.GetValues()));

  // Check that it doesn't change the batch dimension. This check is
  // conservative, for example, when the first dim of the shape is -1 and input
  // tensor shape is not fixed, it is still possible that the reshape doesn't
  // change the batch dim, but as long as there is a possibility that it could
  // change the batch dim, it reject the conversion. The parameters are:
  //
  // * reshape_batch_dim: the value of the first dim of the input shape constant
  // * reshape_dims: all other dims of the input shape constant
  // * input_batch_dim: the value of the first dim of the input tensor to
  //   reshape
  // * input_dims: all other dims of the input tensor to reshape
  //
  // The validation logic is:
  //
  // if input_batch_dim is fixed:
  //   if reshape_batch_dim == input_batch_dim:
  //     ok
  //   elif reshape_batch_dim == -1 (meaning reshape_dims are fixed) and
  //        input_dims are fixed and
  //        prod(input_dims) == prod(reshape_dims)
  //     ok
  //   else:
  //     not ok
  // elif input_dims are fixed:
  //   if reshape_dims are fixed and
  //      prod(input_dims) == prod(reshape_dims):
  //     ok
  //   else:
  //     not ok
  // else:
  //   not ok

  const int input_batch_dim = input_tensor.batch_size();
  const int reshape_batch_dim = weights_ptr[0];
  const nvinfer1::Dims input_dims = input_tensor.GetTrtDims();

  nvinfer1::Dims reshape_dims;
  reshape_dims.nbDims = weights.count() - 1;
  for (int i = 1; i < weights.count(); i++) {
    reshape_dims.d[i - 1] = weights_ptr[i];
  }

  // Check that it doesn't change the batch dimension according to the logic
  // mentioned above.
  bool reshape_may_change_batch_dim = false;
  if (input_batch_dim > 0) {        // Batch size is fixed.
    if (reshape_batch_dim == -1) {  // Other dims of the shape must be fixed.
      if (!HasStaticShape(input_dims) ||
          TrtDimsNumElements(reshape_dims) != TrtDimsNumElements(input_dims)) {
        reshape_may_change_batch_dim = true;
      }
    } else if (reshape_batch_dim != input_batch_dim) {
      reshape_may_change_batch_dim = true;
    }
  } else if (HasStaticShape(input_dims)) {
    if (!HasStaticShape(reshape_dims) ||
        TrtDimsNumElements(reshape_dims) != TrtDimsNumElements(input_dims)) {
      reshape_may_change_batch_dim = true;
    }
  } else {
    reshape_may_change_batch_dim = true;
  }
  VLOG(1) << "input_batch_dim=" << input_batch_dim
          << ", input_dims=" << DebugString(input_dims)
          << "\nreshape_batch_dim=" << reshape_batch_dim
          << ", reshape_dims=" << DebugString(reshape_dims);
  if (reshape_may_change_batch_dim) {
    const string msg = StrCat(
        "Reshape on batch dimension is not supported, at ", node_def.name());
    return errors::Unimplemented(msg);
  }
  if (params->validation_only) return Status::OK();

  // Start conversion.
  const nvinfer1::ITensor* output_tensor = nullptr;
  TF_RETURN_IF_ERROR(params->converter->PrepareTensorForShape(
      input_tensor, reshape_dims, &output_tensor));
  params->outputs->push_back(
      TRT_TensorOrWeights(const_cast<nvinfer1::ITensor*>(output_tensor)));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertConv2D(OpConverterParams* params) {
  return ConvertConv2DHelper(params, ConvolutionType::DEFAULT);
}

tensorflow::Status ConvertConv2DDepthwise(OpConverterParams* params) {
  return ConvertConv2DHelper(params, ConvolutionType::DEPTHWISE_CONV);
}

tensorflow::Status ConvertPool(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  const nvinfer1::ITensor* tensor = inputs.at(0).tensor();
  TFAttrs attrs(node_def);

  int h_index = 2;
  int w_index = 3;
  const auto data_format = attrs.get<string>("data_format");
  if (data_format == "NHWC") {
    h_index = 1;
    w_index = 2;
    TF_RETURN_IF_ERROR(params->converter->TransposeTensor(
        const_cast<nvinfer1::ITensor*>(tensor), {0, 3, 1, 2}, &tensor));
  }

  nvinfer1::PoolingType type;
  if (node_def.op() == "MaxPool") {
    type = nvinfer1::PoolingType::kMAX;
  } else if (node_def.op() == "AvgPool") {
    type = nvinfer1::PoolingType::kAVERAGE;
  } else {
    return tensorflow::errors::Unimplemented("Unsupported pool type: ",
                                             node_def.op());
  }

  const auto tf_stride = attrs.get<std::vector<int>>("strides");
  const nvinfer1::DimsHW stride(tf_stride[h_index], tf_stride[w_index]);

  const auto tf_kernel = attrs.get<std::vector<int>>("ksize");
  const nvinfer1::DimsHW ksize(tf_kernel[h_index], tf_kernel[w_index]);

  auto tensor_dim = tensor->getDimensions();
  std::vector<std::pair<int, int>> padding;
  const string padding_type = attrs.get<string>("padding");
  if (padding_type == "SAME") {
    // This is NCHW tensor with no batch dimension.
    //  1 -> h
    //  2 -> w
    padding = CreateSamePadding(
        stride, ksize,
        {static_cast<int>(tensor_dim.d[1]), static_cast<int>(tensor_dim.d[2])});
  } else if (padding_type == "VALID") {
    padding = {{0, 0}, {0, 0}};
  } else {
    return tensorflow::errors::Unimplemented("Unsupported padding type: ",
                                             padding_type);
  }

  if (padding[0].first != padding[0].second ||
      padding[1].first != padding[1].second) {
    VLOG(2) << "Padding!!!: " << padding[0].first << padding[0].second
            << padding[1].first << padding[1].second;
    auto pad_layer = params->converter->network()->addPadding(
        *const_cast<nvinfer1::ITensor*>(tensor),
        nvinfer1::DimsHW(padding[0].first, padding[1].first),
        nvinfer1::DimsHW(padding[0].second, padding[1].second));
    TFTRT_RETURN_ERROR_IF_NULLPTR(pad_layer, node_def.name());
    padding = {{0, 0}, {0, 0}};
    tensor = pad_layer->getOutput(0);
  }

  nvinfer1::IPoolingLayer* layer = params->converter->network()->addPooling(
      *const_cast<nvinfer1::ITensor*>(tensor), type, ksize);
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());

  layer->setStride(stride);
  layer->setPadding({padding[0].first, padding[1].first});
  layer->setName(node_def.name().c_str());
  const nvinfer1::ITensor* output_tensor = layer->getOutput(0);

  if (data_format == "NHWC") {
    TF_RETURN_IF_ERROR(params->converter->TransposeTensor(
        const_cast<nvinfer1::ITensor*>(output_tensor), {0, 2, 3, 1},
        &output_tensor));
  }
  params->outputs->push_back(
      TRT_TensorOrWeights(const_cast<nvinfer1::ITensor*>(output_tensor)));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertActivation(OpConverterParams* params) {
  const nvinfer1::ITensor* tensor = params->inputs.at(0).tensor();
  nvinfer1::IActivationLayer* layer =
      params->converter->network()->addActivation(
          *const_cast<nvinfer1::ITensor*>(tensor),
          nvinfer1::ActivationType::kRELU);
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, params->node_def.name());
  nvinfer1::ITensor* output_tensor = layer->getOutput(0);
  params->outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertScale(OpConverterParams* params) {
  const auto inputs = params->inputs;
  const auto& node_def = params->node_def;
  if (inputs.size() != 2 || !inputs.at(0).is_tensor() ||
      !inputs.at(1).is_weights()) {
    return tensorflow::errors::Unimplemented(
        "ConvertScale only supports tensor<op>weight: ", node_def.name());
  }

  const nvinfer1::ITensor* tensor = inputs.at(0).tensor();
  TRT_ShapedWeights weights = inputs.at(1).weights();
  if (params->converter->is_fp16()) {
    weights = ConvertFP32ToFP16(params->weight_store, inputs.at(1).weights());
  }

  TRT_ShapedWeights empty_weights(weights.type_);
  TFAttrs attrs(node_def);

  const auto data_format = attrs.get<string>("data_format");
  int channel_index;
  const auto dims = tensor->getDimensions();
  if (data_format == "NHWC") {
    //  1). NHWC is really N+C
    channel_index = dims.nbDims - 1;  // batch dimension is implicit here!
  } else {
    //  2). NCHW is really N+CHW
    channel_index = 0;  // batch dimension is implicit here!
  }

  nvinfer1::Permutation permutation;
  for (int32_t i = 0; i < dims.nbDims; ++i) {
    permutation.order[i] = i;
  }

  if (channel_index >= 0) {
    permutation.order[0] = channel_index;
    permutation.order[channel_index] = 0;
  } else {
    return tensorflow::errors::Unimplemented(
        "TFTRT::BiasAdd cannot apply on batch dimension, at ", node_def.name());
  }

  // TensorRT addScale requires input to be of rank 3, we need to apply
  // transpose as well as reshape
  if (channel_index != 0 || dims.nbDims != 3) {
    nvinfer1::IShuffleLayer* shuffle_layer =
        params->converter->network()->addShuffle(
            *const_cast<nvinfer1::ITensor*>(tensor));
    TFTRT_RETURN_ERROR_IF_NULLPTR(shuffle_layer, node_def.name());
    nvinfer1::Dims reshape_dims;
    reshape_dims.nbDims = 3;
    reshape_dims.d[0] = 0;                          // 0 copy from the input
    reshape_dims.d[1] = dims.nbDims >= 2 ? 0 : 1;   // 0 copy from the input
    reshape_dims.d[2] = dims.nbDims >= 3 ? -1 : 1;  // -1 infer from the rest
    if (channel_index != 0) {
      // maybe we do not need this check. concerned about TRT optimization
      shuffle_layer->setFirstTranspose(permutation);
    }
    shuffle_layer->setReshapeDimensions(reshape_dims);
    tensor = shuffle_layer->getOutput(0);
  }

  nvinfer1::ScaleMode mode = nvinfer1::ScaleMode::kCHANNEL;
  if (weights.shape_.d[0] == 1) {
    mode = nvinfer1::ScaleMode::kUNIFORM;
  }

  nvinfer1::IScaleLayer* layer = params->converter->network()->addScale(
      *const_cast<nvinfer1::ITensor*>(tensor), mode, weights.GetTrtWeights(),
      empty_weights.GetTrtWeights(), empty_weights.GetTrtWeights());
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());

  nvinfer1::ITensor* output_tensor = layer->getOutput(0);

  // restore transpose & reshape
  if (channel_index != 0 || dims.nbDims != 3) {
    nvinfer1::IShuffleLayer* shuffle_layer =
        params->converter->network()->addShuffle(
            *const_cast<nvinfer1::ITensor*>(output_tensor));
    TFTRT_RETURN_ERROR_IF_NULLPTR(shuffle_layer, node_def.name());
    nvinfer1::Dims reshape_dims = dims;
    int tmp = reshape_dims.d[channel_index];
    reshape_dims.d[channel_index] = reshape_dims.d[0];
    reshape_dims.d[0] = tmp;
    shuffle_layer->setReshapeDimensions(reshape_dims);
    if (channel_index != 0) {
      shuffle_layer->setSecondTranspose(permutation);
    }
    output_tensor = shuffle_layer->getOutput(0);
  }

  params->outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

Status GetTensorDimsWithProtoShape(const Tensor& tensor,
                                   int tensor_proto_array_len,
                                   nvinfer1::Dims* dims) {
  if (tensor.dims() > 0) {
    *dims = GetTrtDimsForTensor(tensor);
    if (TrtDimsNumElements(*dims) != tensor_proto_array_len &&
        tensor_proto_array_len != 1) {
      return errors::InvalidArgument(
          "Broadcast on weights only supports kCHANNEL and kUNIFORM");
    }
  } else {
    dims->nbDims = 1;
    // No dimension provided. Flatten it.
    dims->d[0] = tensor_proto_array_len;
    dims->type[0] = nvinfer1::DimensionType::kSPATIAL;
    for (int i = 1; i < nvinfer1::Dims::MAX_DIMS; ++i) {
      dims->d[i] = 0;
    }
  }
  return Status::OK();
}

template <typename CType>
Status TfTensorToTrtWeights(const DataType dtype, const Tensor& tensor,
                            const CType* tensor_proto_array,
                            int tensor_proto_array_len, TrtWeightStore* store,
                            TRT_ShapedWeights* weights) {
  nvinfer1::Dims weight_dims;
  TF_RETURN_IF_ERROR(GetTensorDimsWithProtoShape(tensor, tensor_proto_array_len,
                                                 &weight_dims));
  *weights = store->GetTempWeights(dtype, weight_dims);
  void* dst = const_cast<void*>(weights->GetValues());
  if (tensor_proto_array_len == 1) {
    std::fill_n((CType*)dst, TrtDimsNumElements(weight_dims),
                *tensor_proto_array);
  } else {
    memcpy(dst, tensor_proto_array, weights->size_bytes());
  }
  return Status::OK();
}

tensorflow::Status ConvertConst(OpConverterParams* params) {
  const auto inputs = params->inputs;
  const auto& node_def = params->node_def;
  if (!inputs.empty()) {
    return errors::InvalidArgument(
        "Constant node is expected to have empty input list: ",
        node_def.name());
  }
  TFAttrs attrs(node_def);
  const DataType dtype = attrs.get<tensorflow::DataType>("dtype");
  // We always convert the integer constants to kINT32, since TRT kINT8 is for
  // quantized inference.
  const DataType converted_dtype =
      (dtype == DT_INT16 || dtype == DT_INT8 || dtype == DT_UINT8 ? DT_INT32
                                                                  : dtype);
  nvinfer1::DataType trt_dtype;
  TF_RETURN_IF_ERROR(ConvertDType(converted_dtype, &trt_dtype));

  // Create shaped weights as output
  const auto& tensor_proto = node_def.attr().at("value").tensor();
  tensorflow::Tensor tensor;
  if (!tensor.FromProto(tensor_proto)) {
    return tensorflow::errors::Internal("Cannot parse weight tensor proto: ",
                                        node_def.name());
  }

  TRT_ShapedWeights weights(converted_dtype);
  if (tensor.NumElements() == 0) {
    // Do nothing.
  } else if (!tensor_proto.float_val().empty()) {
    TF_RETURN_IF_ERROR(TfTensorToTrtWeights(
        converted_dtype, tensor, tensor_proto.float_val().begin(),
        tensor_proto.float_val_size(), params->weight_store, &weights));
  } else if (!tensor_proto.int_val().empty()) {
    TF_RETURN_IF_ERROR(TfTensorToTrtWeights(
        converted_dtype, tensor, tensor_proto.int_val().begin(),
        tensor_proto.int_val_size(), params->weight_store, &weights));
  } else if (!tensor_proto.half_val().empty()) {
    // TODO(aaroey): implement fp16 conversion.
    return errors::Unimplemented("fp16 constant is not supported yet.");
  } else if (!tensor_proto.tensor_content().empty()) {
    // TODO(aaroey): fp16 will remain in half format and is not converted to
    // fp32, but the converter currently uses all float weights as fp32. Fix
    // this.
    const auto& content = tensor_proto.tensor_content();
    if (content.size() > 0) {
      const int dtype_size = tensorflow::DataTypeSize(dtype);
      if (content.size() % dtype_size != 0) {
        return errors::FailedPrecondition("Tensor content size ",
                                          content.size(),
                                          " is not a multiple of ", dtype_size);
      }
      nvinfer1::Dims weights_dim;
      TF_RETURN_IF_ERROR(GetTensorDimsWithProtoShape(
          tensor, content.size() / dtype_size, &weights_dim));
      const int64_t size_bytes = TrtDimsNumElements(weights_dim) * dtype_size;
      if (content.size() != size_bytes) {
        return errors::FailedPrecondition(
            "Tensor size and TensorProto content size mismatch: ", size_bytes,
            " vs ", content.size());
      } else if (tensor.NumElements() != content.size() / dtype_size) {
        return errors::FailedPrecondition(
            "Tensor elements count and TensorProto content size mismatch: ",
            tensor.NumElements(), " vs ", content.size() / dtype_size);
      }
      weights =
          params->weight_store->GetTempWeights(converted_dtype, weights_dim);
      if (dtype_size == tensorflow::DataTypeSize(converted_dtype)) {
        port::CopyToArray(content, static_cast<char*>(
                                       const_cast<void*>(weights.GetValues())));
      } else {
        // Copy out the weights as original data type.
        std::vector<uint8_t> temp_weights(content.size());
        port::CopyToArray(content,
                          reinterpret_cast<char*>(temp_weights.data()));
        int32* dst =
            static_cast<int32*>(const_cast<void*>(weights.GetValues()));
        // Copy to the weight store as converted data type.
        if (dtype == DT_INT16) {
          int16* data = reinterpret_cast<int16*>(temp_weights.data());
          std::copy(data, data + tensor.NumElements(), dst);
        } else if (dtype == DT_INT8) {
          int8* data = reinterpret_cast<int8*>(temp_weights.data());
          std::copy(data, data + tensor.NumElements(), dst);
        } else if (dtype == DT_UINT8) {
          uint8* data = reinterpret_cast<uint8*>(temp_weights.data());
          std::copy(data, data + tensor.NumElements(), dst);
        } else {
          return errors::FailedPrecondition(
              "Unexpected data type: ", DataTypeString(dtype),
              " at: ", node_def.name());
        }
      }
    }
  } else {
    return errors::Unimplemented("Not supported constant type, at ",
                                 node_def.name());
  }
  // Pass the output.
  if (!params->validation_only) {
    params->outputs->push_back(TRT_TensorOrWeights(weights));
  }
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertIdentity(OpConverterParams* params) {
  params->outputs->push_back(params->inputs.at(0));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertBinary(OpConverterParams* params) {
  const auto inputs = params->inputs;
  const auto& node_def = params->node_def;
  if (inputs.size() != 2) {
    return tensorflow::errors::FailedPrecondition(
        "Binary ops require two tensor input, at ", node_def.name());
  }

  // Constant folding should have been done by TensorFlow

  if (inputs.at(0).is_weights() && inputs.at(1).is_weights()) {
    return tensorflow::errors::Unimplemented(
        "Constant folding is falled back to TensorFlow, binary op received "
        "both input as constant at: ",
        node_def.name());
  }

  // Try to convert into Scale layer first (for better performance)
  // Since scale layer supports restricted broadcast policy and op types, we
  // allow failure and try to handle it through Elementwise op
  // (BinaryTensorOpTensor)
  Status status = tensorflow::Status::OK();
  if (inputs.at(0).is_tensor() && inputs.at(1).is_weights()) {
    status = BinaryTensorOpWeight(params, inputs.at(0).tensor(),
                                  inputs.at(1).weights(), false);
  } else if (inputs.at(0).is_weights() && inputs.at(1).is_tensor()) {
    status = BinaryTensorOpWeight(params, inputs.at(1).tensor(),
                                  inputs.at(0).weights(), true);
  }
  if ((inputs.at(0).is_tensor() && inputs.at(1).is_tensor()) || !status.ok()) {
    status = BinaryTensorOpTensor(params, inputs.at(0), inputs.at(1));
  }
  return status;
}

tensorflow::Status ConvertUnary(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  static const std::unordered_map<string, nvinfer1::UnaryOperation> ops{
      {"Neg", nvinfer1::UnaryOperation::kNEG},
      {"Exp", nvinfer1::UnaryOperation::kEXP},
      {"Log", nvinfer1::UnaryOperation::kLOG},
      {"Sqrt", nvinfer1::UnaryOperation::kSQRT},
      {"Abs", nvinfer1::UnaryOperation::kABS},
      {"Reciprocal", nvinfer1::UnaryOperation::kRECIP},
  };

  if (inputs.size() != 1) {
    return tensorflow::errors::FailedPrecondition(
        "Unary ops require single tensor input, at ", node_def.name());
  }

  // TODO(jie): check type
  const nvinfer1::ITensor* tensor = nullptr;
  TF_RETURN_IF_ERROR(params->converter->PrepareTensorForShape(
      inputs.at(0), inputs.at(0).GetTrtDims(), &tensor));

  nvinfer1::IUnaryLayer* layer;
  if (node_def.op() == "Rsqrt") {
    layer = params->converter->network()->addUnary(
        *const_cast<nvinfer1::ITensor*>(tensor),
        nvinfer1::UnaryOperation::kSQRT);
    TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());
    tensor = layer->getOutput(0);
    layer = params->converter->network()->addUnary(
        *const_cast<nvinfer1::ITensor*>(tensor),
        nvinfer1::UnaryOperation::kRECIP);
  } else if (ops.count(node_def.op()) != 0) {
    layer = params->converter->network()->addUnary(
        *const_cast<nvinfer1::ITensor*>(tensor), ops.at(node_def.op()));
  } else {
    return tensorflow::errors::InvalidArgument(
        "Binary op: ", node_def.op(), " not supported, at ", node_def.name());
  }

  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());
  nvinfer1::ITensor* output_tensor = layer->getOutput(0);
  params->outputs->push_back(
      TRT_TensorOrWeights(const_cast<nvinfer1::ITensor*>(output_tensor)));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertReduce(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  if (inputs.size() != 2 || !inputs.at(0).is_tensor() ||
      !inputs.at(1).is_weights()) {
    return tensorflow::errors::InvalidArgument(
        "Input expects tensor and weights, at", node_def.name());
  }

  const nvinfer1::ITensor* tensor = inputs.at(0).tensor();
  TRT_ShapedWeights index_list = inputs.at(1).weights();

  TFAttrs attrs(node_def);
  auto index_type = attrs.get<tensorflow::DataType>("Tidx");

  // Only expect to handle INT32 as attributes for now
  if (index_type != tensorflow::DataType::DT_INT32) {
    return tensorflow::errors::Unimplemented("Tidx supports only DT_INT32");
  }

  int axes = 0;
  if (index_list.count() == 0) {
    return tensorflow::errors::InvalidArgument(
        "TRT cannot support reduce on all (batch) dimensions, at",
        node_def.name());
  } else {
    auto index_list_data =
        static_cast<int*>(const_cast<void*>(index_list.GetValues()));
    for (int i = 0; i < index_list.count(); i++) {
      int axis = index_list_data[i];
      if (axis < 0) axis += tensor->getDimensions().nbDims + 1;
      if (axis == 0) {
        return tensorflow::errors::InvalidArgument(
            "TRT cannot reduce at batch dimension, at", node_def.name());
      }
      axes |= (1 << (axis - 1));
    }
  }

  nvinfer1::ReduceOperation reduce_operation;
  if (node_def.op() == "Sum") {
    reduce_operation = nvinfer1::ReduceOperation::kSUM;
  } else if (node_def.op() == "Prod") {
    reduce_operation = nvinfer1::ReduceOperation::kPROD;
  } else if (node_def.op() == "Max") {
    reduce_operation = nvinfer1::ReduceOperation::kMAX;
  } else if (node_def.op() == "Min") {
    reduce_operation = nvinfer1::ReduceOperation::kMIN;
  } else if (node_def.op() == "Mean") {
    reduce_operation = nvinfer1::ReduceOperation::kAVG;
  } else {
    return tensorflow::errors::Unimplemented("Op not supported ", node_def.op(),
                                             " , at ", node_def.name());
  }

  const auto keep_dims = attrs.get<bool>("keep_dims");
  nvinfer1::ILayer* layer = params->converter->network()->addReduce(
      *const_cast<nvinfer1::ITensor*>(tensor), reduce_operation, axes,
      keep_dims);
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());

  params->outputs->push_back(TRT_TensorOrWeights(layer->getOutput(0)));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertPad(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  // TODO(aaroey): make a routine for this check and reuse it.
  if (inputs.size() != 2 || !inputs.at(0).is_tensor() ||
      !inputs.at(1).is_weights()) {
    return tensorflow::errors::InvalidArgument(
        "Input expects tensor and weights, at", node_def.name());
  }

  // Implement tensor binaryOp weight [channel wise] for now;
  const nvinfer1::ITensor* tensor = inputs.at(0).tensor();
  const auto dims = tensor->getDimensions();
  // Restore implicit batch dimension
  const int nb_dims = dims.nbDims + 1;

  TRT_ShapedWeights pads = inputs.at(1).weights();

  TFAttrs attrs(node_def);
  // Padding type here is done through TF type
  //   so I can leverage their EnumToDataType for my cast
  auto padding_type = attrs.get<tensorflow::DataType>("Tpaddings");
  // TODO(jie): handle data type conversion for TRT?

  if (pads.shape_.d[0] != nb_dims || pads.shape_.d[1] != 2) {
    return tensorflow::errors::InvalidArgument(
        "Pad only supports explicit padding on 4 dimensional tensor, at ",
        node_def.name());
  }

  // Only expect to handle INT32 as attributes for now
  if (padding_type != tensorflow::DataType::DT_INT32) {
    return tensorflow::errors::Unimplemented(
        "Tpaddings supports only DT_INT32");
  }
  auto pad_data = static_cast<int*>(const_cast<void*>(pads.GetValues()));

  std::vector<int32_t> pad_index;
  for (int i = 0; i < nb_dims; i++) {
    if (pad_data[2 * i] != 0 || pad_data[2 * i + 1] != 0) {
      pad_index.push_back(i);
    }
  }

  // No padding at all, we should exit
  if (pad_index.size() == 0) {
    params->outputs->push_back(inputs.at(0));
    return tensorflow::Status::OK();
  }

  // Only supports padding on less than 2 axis GIE-2579
  if (pad_index.size() > 2) {
    return tensorflow::errors::InvalidArgument(
        "Padding layer does not support padding on > 2");
  }

  // Padding on batch dimension is not supported
  if (pad_index[0] == 0) {
    return tensorflow::errors::InvalidArgument(
        "Padding layer does not support padding on batch dimension");
  }

  // Not doing the legit thing here. ignoring padding on dim 1 and 3;
  // TODO(jie): implement pad as uff parser
  if (pad_index.size() == 2 && pad_index[0] == 0 && pad_index[1] == 3) {
    return tensorflow::errors::Unimplemented(
        "Padding layer does not support padding on dimension 1 and 3 yet");
  }

  bool legit_pad = true;
  nvinfer1::DimsHW pre_padding(0, 0);
  nvinfer1::DimsHW post_padding(0, 0);

  std::vector<int32_t> permuted_pad_index(pad_index);
  if (pad_index[0] == 1) {
    legit_pad = false;
    TF_RETURN_IF_ERROR(params->converter->TransposeTensor(
        const_cast<nvinfer1::ITensor*>(tensor), {0, 3, 2, 1}, &tensor));
    permuted_pad_index[0] = 3;
  }

  for (size_t i = 0; i < pad_index.size(); i++) {
    int index = pad_index[i];
    if (permuted_pad_index[i] == 2) {
      pre_padding.h() = pad_data[index * 2];
      post_padding.h() = pad_data[index * 2 + 1];
    } else if (permuted_pad_index[i] == 3) {
      pre_padding.w() = pad_data[index * 2];
      post_padding.w() = pad_data[index * 2 + 1];
    }
  }

  nvinfer1::IPaddingLayer* layer = params->converter->network()->addPadding(
      *const_cast<nvinfer1::ITensor*>(tensor), pre_padding, post_padding);
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());
  const nvinfer1::ITensor* output_tensor = layer->getOutput(0);

  if (!legit_pad) {
    TF_RETURN_IF_ERROR(params->converter->TransposeTensor(
        const_cast<nvinfer1::ITensor*>(output_tensor), {0, 3, 2, 1},
        &output_tensor));
  }

  params->outputs->push_back(
      TRT_TensorOrWeights(const_cast<nvinfer1::ITensor*>(output_tensor)));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertConcat(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  // not including the last input (axis) here
  int input_size = static_cast<int>(inputs.size()) - 1;

  if (!inputs.at(0).is_tensor()) {
    return tensorflow::errors::InvalidArgument(
        "Concat in TRT support only Tensor input, at ", node_def.name());
  }

  // We are retrieving the axis
  TRT_ShapedWeights axis = inputs.at(input_size).weights();

  TFAttrs attrs(node_def);
  auto index_type = attrs.get<tensorflow::DataType>("Tidx");

  // TODO(jie): handle data type
  // Only expect to handle INT32 as index attributes for now
  if (index_type != tensorflow::DataType::DT_INT32)
    return tensorflow::errors::Unimplemented("Tidx supports only DT_INT32, at ",
                                             node_def.name());

  int index = *(static_cast<int*>(const_cast<void*>(axis.GetValues())));

  // TODO(jie): early termination with no-op (attr_size==1)

  auto dim = inputs.at(0).tensor()->getDimensions();
  // dimension check
  if (index > dim.nbDims + 1) {
    return tensorflow::errors::InvalidArgument(
        "Concatenate on axis out of dimension range, at ", node_def.name());
  }
  if (index == 0) {
    return tensorflow::errors::InvalidArgument(
        "Concatenate on batch dimension not supported, at ", node_def.name());
  }
  if (index < 0) {
    index = dim.nbDims + index + 1;
  }

  std::vector<nvinfer1::ITensor const*> inputs_vec;
  // Shap chack (all input tensor should have same shape)
  // starting from 0 since we are probably also doing transpose here;
  for (int i = 0; i < input_size; i++) {
    auto tensor_i = inputs.at(i).tensor();
    auto dim_i = tensor_i->getDimensions();
    if (dim_i.nbDims != dim.nbDims) {
      return tensorflow::errors::InvalidArgument(
          "Concatenate receives inputs with inconsistent dimensions, at ",
          node_def.name());
    }
    for (int j = 0; j < dim.nbDims; j++) {
      // check dimension consistency on non-concatenate axis
      if (j != index - 1 && dim_i.d[j] != dim.d[j]) {
        return tensorflow::errors::InvalidArgument(
            "Concatenate receives inputs with inconsistent shape, at",
            node_def.name());
      }
    }

    inputs_vec.push_back(tensor_i);
  }

  // nvinfer1::ITensor const* tensor = inputs.at(0).tensor();
  nvinfer1::IConcatenationLayer* layer =
      params->converter->network()->addConcatenation(
          const_cast<nvinfer1::ITensor* const*>(inputs_vec.data()),
          inputs_vec.size());
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());
  layer->setAxis(index - 1);
  nvinfer1::ITensor* output_tensor = layer->getOutput(0);
  params->outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertFusedBatchNorm(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  TFAttrs attrs(node_def);
  float epsilon = attrs.get<float>("epsilon");
  auto data_format = attrs.get<string>("data_format");
  if (data_format != "NCHW") {
    return tensorflow::errors::Unimplemented(
        "only data_format=NCHW is supported, at " + node_def.name());
  }
  bool is_training = attrs.get<bool>("is_training");
  if (is_training) {
    return tensorflow::errors::Unimplemented(
        "only is_training=false is supported, at " + node_def.name());
  }
  nvinfer1::ITensor const* tensor = inputs.at(0).tensor();

  //  Check parameter types
  auto parameter_type = inputs.at(1).weights().type_;
  if ((parameter_type != tensorflow::DataType::DT_FLOAT) &&
      (parameter_type != tensorflow::DataType::DT_HALF)) {
    return tensorflow::errors::Unimplemented(
        "only float32 or float16 weight data type is supported, for node " +
        node_def.name() + " got " + tensorflow::DataTypeString(parameter_type));
  }
  for (int i = 1; i < 5; i++) {
    if (inputs.at(i).weights().type_ != parameter_type) {
      return tensorflow::errors::Unimplemented(
          "Inconsistent parameter type for batchnormis not supported, at: " +
          node_def.name());
    }
  }

  TRT_ShapedWeights dummy_power_weights(parameter_type);
  size_t nweight = 0;
  for (int i = 1; i < 5; i++) {
    nweight = std::max(nweight, (size_t)inputs.at(i).weights().count());
  }
  TRT_ShapedWeights* ptr_shape_weights = nullptr;
  for (int i = 1; i < 5; i++) {
    if (inputs.at(i).weights().count() == nweight) {
      ptr_shape_weights =
          const_cast<TRT_ShapedWeights*>(&(inputs.at(i).weights()));
    } else if (inputs.at(i).weights().count() != 1) {
      return tensorflow::errors::InvalidArgument(
          "Inconsistent batchnorm parameter count, at: " + node_def.name());
    }
  }
  //  We could technically have two weights with different shape.
  //  that requires two addScale op, arguably less performant
  TRT_ShapedWeights combined_scale_weights =
      params->weight_store->GetTempWeights(*ptr_shape_weights);
  TRT_ShapedWeights combined_offset_weights =
      params->weight_store->GetTempWeights(*ptr_shape_weights);

  const Eigen::half* cast_vals_array[4];
  const float* vals_array[4];
  for (int j = 0; j < 4; j++) {
    cast_vals_array[j] =
        static_cast<Eigen::half const*>(inputs.at(j + 1).weights().GetValues());
    vals_array[j] =
        static_cast<float const*>(inputs.at(j + 1).weights().GetValues());
  }
  Eigen::half* cast_combined_scale_vals = const_cast<Eigen::half*>(
      static_cast<Eigen::half const*>(combined_scale_weights.GetValues()));
  Eigen::half* cast_combined_offset_vals = const_cast<Eigen::half*>(
      static_cast<Eigen::half const*>(combined_offset_weights.GetValues()));
  float* combined_scale_vals = const_cast<float*>(
      static_cast<float const*>(combined_scale_weights.GetValues()));
  float* combined_offset_vals = const_cast<float*>(
      static_cast<float const*>(combined_offset_weights.GetValues()));

  for (size_t i = 0; i < nweight; ++i) {
    float batchnorm_data[4];
    for (int j = 0; j < 4; j++) {
      if (inputs.at(j + 1).weights().count() != 1) {
        if (parameter_type == tensorflow::DT_FLOAT) {
          batchnorm_data[j] = vals_array[j][i];
        } else if (parameter_type == tensorflow::DT_HALF) {
          batchnorm_data[j] =
              Eigen::half_impl::half_to_float(cast_vals_array[j][i]);
        }
      } else {
        if (parameter_type == tensorflow::DT_FLOAT) {
          batchnorm_data[j] = vals_array[j][0];
        } else if (parameter_type == tensorflow::DT_HALF) {
          batchnorm_data[j] =
              Eigen::half_impl::half_to_float(cast_vals_array[j][0]);
        }
      }
    }
    float scale = batchnorm_data[0];
    float offset = batchnorm_data[1];
    float mean = batchnorm_data[2];
    float variance = batchnorm_data[3];
    float combined_scale_val = scale / sqrtf(variance + epsilon);
    float combined_offset_val = offset - mean * combined_scale_val;
    if (parameter_type == tensorflow::DT_FLOAT) {
      combined_scale_vals[i] = combined_scale_val;
      combined_offset_vals[i] = combined_offset_val;
    } else if (parameter_type == tensorflow::DT_HALF) {
      cast_combined_scale_vals[i] = Eigen::half(combined_scale_val);
      cast_combined_offset_vals[i] = Eigen::half(combined_offset_val);
    }
  }

  nvinfer1::ScaleMode mode = nweight == 1 ? nvinfer1::ScaleMode::kUNIFORM
                                          : nvinfer1::ScaleMode::kCHANNEL;
  nvinfer1::IScaleLayer* layer = params->converter->network()->addScale(
      *const_cast<nvinfer1::ITensor*>(tensor), mode,
      combined_offset_weights.GetTrtWeights(),
      combined_scale_weights.GetTrtWeights(),
      dummy_power_weights.GetTrtWeights());
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());
  nvinfer1::ITensor* output_tensor = layer->getOutput(0);
  params->outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertMatMulHelper(OpConverterParams* params,
                                       TRT_TensorOrWeights tensor_input,
                                       TRT_ShapedWeights weights_raw,
                                       bool transpose_weight,
                                       string node_name) {
  nvinfer1::ITensor* output_tensor;
  if (!tensor_input.is_tensor()) {
    return tensorflow::errors::InvalidArgument("Input 0 expects tensor");
  }
  const nvinfer1::ITensor* tensor = tensor_input.tensor();

  TRT_ShapedWeights weights(weights_raw.type_);
  if (transpose_weight) {
    weights = weights_raw;
  } else {
    weights = params->weight_store->GetTempWeights(weights_raw);
    ReorderCKtoKC(weights_raw, &weights);
  }
  TRT_ShapedWeights biases(weights.type_);

  int noutput = weights.shape_.d[0];

  auto input_dim = tensor->getDimensions();
  while (input_dim.nbDims != 3) {
    input_dim.d[input_dim.nbDims++] = 1;
  }
  TF_RETURN_IF_ERROR(params->converter->PrepareTensorForShape(
      tensor_input, input_dim, &tensor));

  nvinfer1::IFullyConnectedLayer* layer =
      params->converter->network()->addFullyConnected(
          *const_cast<nvinfer1::ITensor*>(tensor), noutput,
          weights.GetTrtWeights(), biases.GetTrtWeights());
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_name);
  output_tensor = layer->getOutput(0);

  const nvinfer1::ITensor* temp_tensor = nullptr;
  auto output_dim = output_tensor->getDimensions();
  output_dim.nbDims = 1;
  TF_RETURN_IF_ERROR(params->converter->PrepareTensorForShape(
      TRT_TensorOrWeights(output_tensor), output_dim, &temp_tensor));
  output_tensor = const_cast<nvinfer1::ITensor*>(temp_tensor);
  params->outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

// inputs are both two dimensional (tensorflow::ops::MatMul)
tensorflow::Status ConvertMatMul(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  if (inputs.size() != 2 || !inputs.at(0).is_tensor() ||
      !inputs.at(1).is_weights()) {
    return errors::InvalidArgument("Input expects tensor and weights, at ",
                                   node_def.name());
  }

  TFAttrs attrs(node_def);
  // TODO(jie): INT32 should be converted?
  tensorflow::DataType tf_dtype = attrs.get<tensorflow::DataType>("T");
  if (tf_dtype != DataType::DT_FLOAT && tf_dtype != DataType::DT_HALF) {
    return tensorflow::errors::Unimplemented(
        "data type is not supported, for node " + node_def.name() + " got " +
        tensorflow::DataTypeString(tf_dtype));
  }
  bool transpose_a = attrs.get<bool>("transpose_a");
  bool transpose_b = attrs.get<bool>("transpose_b");

  // FullyConnected:
  if (transpose_a) {
    return tensorflow::errors::Internal(
        "Transpose_a is not supported for TensorRT FullyConnected (op: " +
        node_def.op() + "), at: " + node_def.name());
  }
  return ConvertMatMulHelper(params, inputs.at(0), inputs.at(1).weights(),
                             transpose_b, node_def.name());
}

tensorflow::Status ConvertBatchMatMul(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  TFAttrs attrs(node_def);

  // TODO(jie): INT32 should be converted?
  tensorflow::DataType tf_dtype = attrs.get<tensorflow::DataType>("T");
  if (tf_dtype != tensorflow::DataType::DT_FLOAT &&
      tf_dtype != tensorflow::DataType::DT_HALF) {
    return tensorflow::errors::Unimplemented(
        "data type is not supported, for node " + node_def.name() + " got " +
        tensorflow::DataTypeString(tf_dtype));
  }

  bool transpose_a = attrs.get<bool>("adj_x");
  bool transpose_b = attrs.get<bool>("adj_y");

  auto dims = inputs.at(0).GetTrtDims();
  if (dims.nbDims == 1) {  // NC * CK is only supported through fully connected
    if (transpose_a == false && inputs.at(0).is_tensor() &&
        inputs.at(1).is_weights()) {
      return ConvertMatMulHelper(params, inputs.at(0), inputs.at(1).weights(),
                                 transpose_b, node_def.name());
    } else {
      return tensorflow::errors::InvalidArgument(
          "Invalid configuration for MatMul, at: " + node_def.name());
    }
  }

  const nvinfer1::ITensor* tensor_l;
  const nvinfer1::ITensor* tensor_r;
  auto dims_l = inputs.at(0).GetTrtDims();
  auto dims_r = inputs.at(1).GetTrtDims();
  if (inputs.at(0).is_weights()) {
    if (inputs.at(0).GetTrtDims().d[0] != 1) {
      return tensorflow::errors::InvalidArgument(
          "Input 0 as weight assumes broadcast across batch for MatMul, at: " +
          node_def.name());
    } else {
      for (int i = 0; i < dims_l.nbDims - 1; i++) {
        dims_l.d[i] = dims_l.d[i + 1];
      }
      dims_l.nbDims--;
    }
  }
  if (inputs.at(1).is_weights()) {
    if (inputs.at(1).GetTrtDims().d[0] != 1) {
      return tensorflow::errors::InvalidArgument(
          "Input 1 as weight assumes broadcast across batch for MatMul, at: " +
          node_def.name());
    } else {
      for (int i = 0; i < dims_r.nbDims - 1; i++) {
        dims_r.d[i] = dims_r.d[i + 1];
      }
      dims_r.nbDims--;
    }
  }
  TF_RETURN_IF_ERROR(params->converter->PrepareTensorForShape(
      inputs.at(0), dims_l, &tensor_l));
  TF_RETURN_IF_ERROR(params->converter->PrepareTensorForShape(
      inputs.at(1), dims_r, &tensor_r));

  nvinfer1::IMatrixMultiplyLayer* layer =
      params->converter->network()->addMatrixMultiply(
          *const_cast<nvinfer1::ITensor*>(tensor_l), transpose_a,
          *const_cast<nvinfer1::ITensor*>(tensor_r), transpose_b);
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());
  nvinfer1::ITensor* output_tensor = layer->getOutput(0);
  params->outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertSoftmax(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  const nvinfer1::ITensor* tensor = inputs.at(0).tensor();

  int nbDims = tensor->getDimensions().nbDims;
  if (nbDims == 0) {
    return tensorflow::errors::InvalidArgument(
        "TensorRT Softmax cannot apply on batch dimension, at" +
        node_def.name());
  }
  nvinfer1::ISoftMaxLayer* layer = params->converter->network()->addSoftMax(
      *const_cast<nvinfer1::ITensor*>(tensor));
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());
  // Tensorflow SoftMax assumes applying softmax on the last dimension.
  layer->setAxes(1 << (nbDims - 1));

  nvinfer1::ITensor* output_tensor = layer->getOutput(0);
  params->outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertTopK(OpConverterParams* params) {
  const auto& inputs = params->inputs;
  const auto& node_def = params->node_def;
  const nvinfer1::ITensor* tensor = inputs.at(0).tensor();

  int nbDims = tensor->getDimensions().nbDims;
  if (nbDims == 0) {
    return tensorflow::errors::InvalidArgument(
        "TensorRT TopK cannot apply on batch dimension, at" + node_def.name());
  }

  TRT_ShapedWeights k_w = inputs.at(1).weights();
  int k = *(static_cast<int*>(const_cast<void*>(k_w.GetValues())));

  nvinfer1::TopKOperation op;
  uint32_t reducedAxes = 0;
  if (node_def.op() == "TopKV2") {
    op = nvinfer1::TopKOperation::kMAX;
    reducedAxes |= 1 << (nbDims - 1);
  } else {
    return tensorflow::errors::Unimplemented(
        "Operation: " + node_def.op() +
        " not implemented, at: " + node_def.name());
  }

  nvinfer1::ITopKLayer* layer = params->converter->network()->addTopK(
      *const_cast<nvinfer1::ITensor*>(tensor), op, k, reducedAxes);
  TFTRT_RETURN_ERROR_IF_NULLPTR(layer, node_def.name());

  nvinfer1::ITensor* output_value_tensor = layer->getOutput(0);
  nvinfer1::ITensor* output_indices_tensor = layer->getOutput(1);
  params->outputs->push_back(TRT_TensorOrWeights(output_value_tensor));
  params->outputs->push_back(TRT_TensorOrWeights(output_indices_tensor));
  return tensorflow::Status::OK();
}

void TrtNodeValidator::RegisterOpValidators() {
  // TODO(laigd): support all op types.
  op_validators_["Const"] = ConvertConst;
  op_validators_["Transpose"] = ConvertTranspose;
  op_validators_["Reshape"] = ConvertReshape;
  op_validators_["MatMul"] = ConvertMatMul;
}

void Converter::RegisterOpConverters() {
  // vgg_16 slim implementation
  op_registry_["Conv2D"] = ConvertConv2D;
  op_registry_["DepthwiseConv2dNative"] = ConvertConv2DDepthwise;
  op_registry_["Relu"] = ConvertActivation;
  op_registry_["MaxPool"] = ConvertPool;
  op_registry_["AvgPool"] = ConvertPool;
  op_registry_["BiasAdd"] = ConvertScale;
  op_registry_["Const"] = ConvertConst;
  // TODO(ben,jie): this is a temp hack.
  op_registry_["Identity"] = ConvertIdentity;  // Identity should be removed
  op_registry_["Snapshot"] = ConvertIdentity;  // Snapshot should be removed

  // resnet_50_v1 slim implementation
  op_registry_["Add"] = ConvertBinary;
  op_registry_["Mul"] = ConvertBinary;
  op_registry_["Sub"] = ConvertBinary;
  op_registry_["Pad"] = ConvertPad;

  op_registry_["ConcatV2"] = ConvertConcat;
  op_registry_["FusedBatchNorm"] = ConvertFusedBatchNorm;
  op_registry_["FusedBatchNormV2"] = ConvertFusedBatchNorm;

  op_registry_["Div"] = ConvertBinary;
  op_registry_["RealDiv"] = ConvertBinary;

  op_registry_["Rsqrt"] = ConvertUnary;
  op_registry_["Reciprocal"] = ConvertUnary;
  op_registry_["Exp"] = ConvertUnary;
  op_registry_["Log"] = ConvertUnary;
  op_registry_["Sqrt"] = ConvertUnary;
  op_registry_["Abs"] = ConvertUnary;
  op_registry_["Neg"] = ConvertUnary;

  op_registry_["Transpose"] = ConvertTranspose;
  op_registry_["Reshape"] = ConvertReshape;

  op_registry_["Sum"] = ConvertReduce;
  op_registry_["Prod"] = ConvertReduce;
  op_registry_["Max"] = ConvertReduce;
  op_registry_["Min"] = ConvertReduce;
  op_registry_["Mean"] = ConvertReduce;
  op_registry_["Maximum"] = ConvertBinary;
  op_registry_["Minimum"] = ConvertBinary;
  op_registry_["Softmax"] = ConvertSoftmax;
  op_registry_["MatMul"] = ConvertMatMul;
  op_registry_["BatchMatMul"] = ConvertBatchMatMul;
  op_registry_["TopKV2"] = ConvertTopK;

  plugin_converter_ = ConvertPlugin;
}

tensorflow::Status ConvertGraphDefToEngine(
    const tensorflow::GraphDef& gdef, int precision_mode, int max_batch_size,
    size_t max_workspace_size_bytes,
    const std::vector<tensorflow::PartialTensorShape>& input_shapes,
    Logger* logger, nvinfer1::IGpuAllocator* allocator,
    TRTInt8Calibrator* calibrator,
    TrtUniquePtrType<nvinfer1::ICudaEngine>* engine,
    bool* convert_successfully) {
  engine->reset();
  if (convert_successfully) *convert_successfully = false;

  // Create the builder.
  TrtUniquePtrType<nvinfer1::IBuilder> builder(
      nvinfer1::createInferBuilder(*logger));
  builder->setMaxBatchSize(max_batch_size);
  builder->setMaxWorkspaceSize(max_workspace_size_bytes);
  builder->setGpuAllocator(allocator);
  if (precision_mode == FP16MODE) {
    builder->setHalf2Mode(true);
  } else if (precision_mode == INT8MODE) {
    builder->setInt8Mode(true);
    builder->setInt8Calibrator(calibrator);
  }

  // Create the network.
  auto trt_network =
      TrtUniquePtrType<nvinfer1::INetworkDefinition>(builder->createNetwork());
  if (!trt_network) {
    return tensorflow::errors::Internal(
        "Failed to create TensorRT network object");
  }

  // Build the network
  VLOG(1) << "Starting engine conversion ";
  Converter converter(trt_network.get(), precision_mode == FP16MODE);
  std::vector<std::pair<string, string>> output_tensors;
  // Graph nodes are already topologically sorted during construction
  for (const auto& node_def : gdef.node()) {
    string node_name = node_def.name();
    VLOG(2) << "Converting op name=" << node_name << ", op=" << node_def.op();
    if (tensorflow::str_util::StartsWith(node_name, kInputPHName) &&
        (node_def.op() == "Placeholder")) {
      int32 slot_number = -1;
      if (!tensorflow::strings::safe_strto32(
              node_name.c_str() + strlen(kInputPHName), &slot_number)) {
        return tensorflow::errors::InvalidArgument(
            "Failed to parse slot number from ", node_name);
      }
      nvinfer1::DataType dtype;
      auto shape = input_shapes.at(slot_number);
      auto status = ValidateInputProperties(
          shape, node_def.attr().at("dtype").type(), &dtype);
      if (!status.ok()) {
        const string error_message =
            StrCat("Validation failed for ", node_name, " and input slot ",
                   slot_number, ": ", status.error_message());
        LOG(WARNING) << error_message;
        return Status(status.code(), error_message);
      }

      nvinfer1::Dims input_dim;
      for (int i = 1; i < shape.dims(); i++) {
        input_dim.d[i - 1] = shape.dim_size(i);
      }
      input_dim.nbDims = shape.dims() - 1;
      VLOG(2) << "Adding engine input tensor " << node_name << " with shape "
              << DebugString(input_dim);
      // TODO(laigd): the conversion should always happen at runtime where all
      // the shapes are known, and we can provide a mode to generate the
      // engines offline, by calling sess.run() and cache/serialize the engines.
      TF_RETURN_IF_ERROR(converter.AddInputTensor(node_name, dtype, input_dim,
                                                  shape.dim_size(0)));
    } else if (tensorflow::str_util::StartsWith(node_name, kOutputPHName) &&
               (node_def.op() == "Identity")) {
      int32 slot_number = -1;
      if (!tensorflow::strings::safe_strto32(
              node_name.c_str() + strlen(kOutputPHName), &slot_number)) {
        return tensorflow::errors::InvalidArgument(
            "Failed to parse slot number from ", node_name);
      }
      if (output_tensors.size() <= slot_number) {
        output_tensors.resize(slot_number + 1);
      }
      output_tensors.at(slot_number) = {node_def.input(0), node_name};
    } else {
      VLOG(2) << "Converting node: " << node_def.name() << " , "
              << node_def.op();
      TF_RETURN_IF_ERROR(converter.ConvertNode(node_def));
    }
  }
  TF_RETURN_IF_ERROR(converter.RenameAndMarkOutputTensors(output_tensors));
  if (convert_successfully) *convert_successfully = true;

  // Build the engine.
  VLOG(1) << "Starting engine creation";
  engine->reset(builder->buildCudaEngine(*converter.network()));
  if (engine->get() == nullptr) {
    return tensorflow::errors::Internal("Failed to build TensorRT engine");
  }
  VLOG(1) << "Finished conversion";
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertSegmentToGraphDef(
    const tensorflow::Graph* graph,
    const tensorflow::grappler::GraphProperties& graph_properties,
    const std::set<string>& subgraph_node_names,
    const std::vector<int>& subgraph_node_ids,  // In topological order
    std::vector<EngineConnection>* connections,
    tensorflow::GraphDef* segment_def, string* common_scope) {
  std::set<string> marker_nodes;
  // Update connection shapes/data types and add corresponding input/output
  // nodes in the segment graphdef.
  for (size_t i = 0; i < connections->size(); ++i) {
    auto& connection = connections->at(i);
    if (connection.is_control_edge()) continue;
    auto outside_node = graph->FindNodeId(connection.outside_id);
    if (!outside_node) {
      // This should never happen, unless the original graph is problematic.
      return tensorflow::errors::NotFound(
          "Cannot find node with id ", connection.outside_id, " in the graph.");
    }
    // Updates the shape and data types of input/output connections.
    tensorflow::DataType dtype;
    tensorflow::PartialTensorShape partial_shape;
    if (connection.is_input_edge) {
      GetOutputProperties(graph_properties,
                          graph->FindNodeId(connection.outside_id),
                          connection.outside_port, &partial_shape, &dtype);
      connection.outside_shape = partial_shape;
    } else {
      GetInputProperties(graph_properties,
                         graph->FindNodeId(connection.outside_id),
                         connection.outside_port, &partial_shape, &dtype);
      connection.inside_shape = partial_shape;
    }
    connection.connection_type = dtype;

    // Add dummy input/output nodes to the segment graphdef.
    if (connection.is_input_edge) {
      const string node_name = StrCat(kInputPHName, connection.port_number);
      if (marker_nodes.count(node_name)) {
        VLOG(1) << "Reusing input " << node_name << " for the edge "
                << connection.outside_node_name << ":"
                << connection.outside_port << " -> "
                << connection.inside_node_name << ":" << connection.inside_port;
        continue;
      }
      marker_nodes.insert(node_name);
      auto seg_node = segment_def->add_node();
      tensorflow::NodeDefBuilder builder(node_name, "Placeholder");
      auto status = builder.Attr("shape", partial_shape)
                        .Attr("dtype", dtype)
                        .Finalize(seg_node);
      VLOG(1) << "Constructing input " << node_name << " for the edge "
              << connection.outside_node_name << ":" << connection.outside_port
              << " -> " << connection.inside_node_name << ":"
              << connection.inside_port;
    } else {
      const string node_name = StrCat(kOutputPHName, connection.port_number);
      if (marker_nodes.count(node_name)) {
        VLOG(1) << "Reusing output " << node_name << " for the edge "
                << connection.inside_node_name << ":" << connection.inside_port
                << " -> " << connection.outside_node_name << ":"
                << connection.outside_port;
        continue;
      }
      marker_nodes.insert(node_name);
      auto seg_node = segment_def->add_node();
      tensorflow::NodeDefBuilder builder(node_name, "Identity");
      auto status = builder.Input(connection.inside_node_name, 0, dtype)
                        .Finalize(seg_node);
      VLOG(1) << "Constructing output " << node_name << " for the edge "
              << connection.inside_node_name << ":" << connection.inside_port
              << " -> " << connection.outside_node_name << ":"
              << connection.outside_port;
    }
  }  // for each connection.

  std::unordered_map<int, int> old_to_new_id_map;
  // Copy internal nodes to new graphdef
  string local_scope = graph->FindNodeId(*subgraph_node_ids.begin())->name();
  for (const auto node_id : subgraph_node_ids) {
    const auto node = graph->FindNodeId(node_id);
    local_scope = GetCommonNameScope(local_scope, node->name());
    old_to_new_id_map[node_id] = segment_def->node_size();
    auto snode = segment_def->add_node();
    snode->CopyFrom(node->def());
    VLOG(2) << "Copying " << snode->name() << " to subgraph";
  }
  // Update the inputs of the new input nodes to point to placeholder nodes.
  for (int i = 0; i < connections->size(); ++i) {
    auto& connection = connections->at(i);
    if (connection.is_control_edge() || !connection.is_input_edge) continue;
    auto snode =
        segment_def->mutable_node(old_to_new_id_map[connection.inside_id]);
    const string placeholder_name =
        StrCat(kInputPHName, connection.port_number);
    VLOG(1) << "Updating " << snode->name() << ":" << connection.inside_port
            << " from " << snode->input(connection.inside_port) << " to "
            << placeholder_name;
    snode->set_input(connection.inside_port, placeholder_name);
  }
  // Remove control inputs that are not inside the segment.
  for (int i = 0; i < segment_def->node_size(); ++i) {
    auto snode = segment_def->mutable_node(i);
    const int input_size = snode->input_size();
    int input_idx = 0;
    int actual_input_idx = 0;
    while (input_idx < input_size) {
      TensorId input = ParseTensorName(snode->input(input_idx));
      if (!subgraph_node_names.count(
              string(input.first.data(), input.first.size())) &&
          !str_util::StartsWith(input.first, kInputPHName)) {
        if (input.second == Graph::kControlSlot) {
          VLOG(1) << "... removing control inputs " << input.first
                  << " from subgraph.";
          ++input_idx;
          continue;
        } else {
          return tensorflow::errors::InvalidArgument(
              "Found non control input outside the segment that is not an "
              "engine connection to ",
              snode->name(), ": ", input.first);
        }
      }
      if (actual_input_idx != input_idx) {
        snode->set_input(actual_input_idx, snode->input(input_idx));
      }
      ++input_idx;
      ++actual_input_idx;
    }
    for (int remove = input_size - actual_input_idx; remove > 0; --remove) {
      snode->mutable_input()->RemoveLast();
    }
  }
  *common_scope = local_scope;
  VLOG(0) << "Segment @scope '" << local_scope << "', converted to graph";
  return tensorflow::Status::OK();
}

bool InputEdgeValidator::operator()(const tensorflow::Edge* in_edge) const {
  if (in_edge->IsControlEdge()) return true;
  PartialTensorShape shape;
  tensorflow::DataType dtype;
  GetOutputProperties(graph_properties_, in_edge->src(), in_edge->src_output(),
                      &shape, &dtype);
  nvinfer1::DataType trt_dtype;
  Status status = ValidateInputProperties(shape, dtype, &trt_dtype);
  if (!status.ok()) {
    VLOG(1) << "--> Need to remove input node " << in_edge->dst()->name()
            << ": " << status;
    return false;
  }


  if (in_edge->src()->type_string() != "Const" &&
      // Single dimensional input tensor is not supported since the first
      // dimension is treated as batch dimension.
      shape.dims() < 2) {
    VLOG(1) << "--> Need to remove input node " << in_edge->dst()->name()
            << " which has an input at port " << in_edge->dst_input() << " with"
            << " #dim<2"
            << " and is not a const: " << shape;
    return false;
  }
  return true;
}

bool OutputEdgeValidator::operator()(const tensorflow::Edge* out_edge) const {
  if (out_edge->IsControlEdge()) return true;
  if (out_edge->src()->type_string() == "Const") {
    VLOG(1) << "--> Need to remove output node " << out_edge->src()->name()
            << " which is a Const.";
    return false;
  }
  return true;
}

}  // namespace convert
}  // namespace tensorrt
}  // namespace tensorflow

#endif  // GOOGLE_TENSORRT
#endif  // GOOGLE_CUDA
