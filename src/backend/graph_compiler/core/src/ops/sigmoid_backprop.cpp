/*******************************************************************************
 * Copyright 2022 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/
#include "sigmoid_backprop.hpp"
#include <compiler/ir/graph/fusible_op.hpp>

namespace sc {
namespace ops {

sigmoid_backprop_op::sigmoid_backprop_op(
        const std::vector<graph_tensor_ptr> &ins,
        const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs) {
    COMPILE_ASSERT(ins.size() == 2, "Wrong op input size.\n");
    info_.inputs_ = ins;
    if (outs.empty()) {
        info_.outputs_.emplace_back(
                std::make_shared<graph_tensor>(this, ins[0]->details_));
    } else {
        info_.outputs_ = outs;
    }
    attrs_ = attrs;
    op_name_ = "sigmoid_backprop";
}

std::shared_ptr<sc_graph_t> sigmoid_backprop_op::get_graph() {
    auto graph = std::make_shared<sc_graph_t>();
    // create new input logical tensors
    std::vector<graph_tensor_ptr> inputs, outputs;
    inputs = remake_logical_tensors(info_.inputs_);
    outputs = remake_logical_tensors(info_.outputs_);

    // input
    graph->make_input(inputs);
    bool is_bf16
            = info_.inputs_[0]->details_.dtype_.is_etype(sc_data_etype::BF16);

    graph_tensor_ptr inputs0 = inputs[0];
    graph_tensor_ptr inputs1 = inputs[1];
    if (is_bf16) {
        auto cast0 = graph->make(
                "cast", {inputs[0]}, {}, {{"dtype", datatypes::f32}});
        inputs0 = cast0->get_outputs()[0];
    }
    // sigmoid_grad = sigmoid(x) - sigmoid(x)*sigmoid(x)
    // if "use_dst" is true, inputs0 is the result of forward, which is
    // sigmoid(x). otherwise, inputs0 is the src of forward, we need to
    // calculate sigmoid(x) by ourselves
    sc_op_ptr mul0, sub, mul1;
    if (attrs_.get_or_else("use_dst", false)) {
        mul0 = graph->make("mul", {inputs0, inputs0}, {}, {});
        sub = graph->make("sub", {inputs0, mul0->get_outputs()[0]}, {}, {});
    } else {
        auto sigmoid = graph->make("sigmoid", {inputs0}, {}, {});
        mul0 = graph->make("mul",
                {sigmoid->get_outputs()[0], sigmoid->get_outputs()[0]}, {}, {});
        sub = graph->make("sub",
                {sigmoid->get_outputs()[0], mul0->get_outputs()[0]}, {}, {});
    }

    if (is_bf16) {
        sub = graph->make(
                "cast", sub->get_outputs(), {}, {{"dtype", datatypes::bf16}});
    }
    mul1 = graph->make("mul", {sub->get_outputs()[0], inputs1}, {}, {});
    // output
    graph->make_output(mul1->get_outputs());
    return graph;
}

void sigmoid_backprop_op::query_format(context_ptr ctx,
        std::vector<std::vector<sc_data_format_t>> &in_formats,
        std::vector<std::vector<sc_data_format_t>> &out_formats) {}

} // namespace ops

OP_REGISTER(ops::sigmoid_backprop_op, sigmoid_backprop)
} // namespace sc
