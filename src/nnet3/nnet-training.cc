// nnet3/nnet-training.cc

// Copyright      2015    Johns Hopkins University (author: Daniel Povey)

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "nnet3/nnet-training.h"
#include "nnet3/nnet-utils.h"

namespace kaldi {
namespace nnet3 {

NnetTrainer::NnetTrainer(const NnetTrainerOptions &config,
                         Nnet *nnet):
    config_(config),
    nnet_(nnet),
    compiler_(*nnet, config_.optimize_config),
    num_minibatches_processed_(0) {
  if (config.zero_component_stats)
    ZeroComponentStats(nnet);
  if (config.momentum == 0.0 && config.max_param_change == 0.0) {
    delta_nnet_= NULL;
  } else {
    KALDI_ASSERT(config.momentum >= 0.0 &&
                 config.max_param_change >= 0.0);
    delta_nnet_ = nnet_->Copy();
    bool is_gradient = false;  // setting this to true would disable the
                               // natural-gradient updates.
    SetZero(is_gradient, delta_nnet_);
  }
}


void NnetTrainer::Train(const NnetExample &eg) {
  bool need_model_derivative = true;
  ComputationRequest request;
  GetComputationRequest(*nnet_, eg, need_model_derivative,
                        config_.store_component_stats,
                        &request);
  const NnetComputation *computation = compiler_.Compile(request);

  NnetComputer computer(config_.compute_config, *computation,
                        *nnet_,
                        (delta_nnet_ == NULL ? nnet_ : delta_nnet_));
  // give the inputs to the computer object.
  computer.AcceptInputs(*nnet_, eg.io);
  computer.Forward();

  this->ProcessOutputs(eg, &computer);
  computer.Backward();

  if (delta_nnet_ != NULL) {
    BaseFloat scale = (1.0 - config_.momentum);
    if (config_.max_param_change != 0.0) {
      BaseFloat param_delta =
          std::sqrt(DotProduct(*delta_nnet_, *delta_nnet_)) * scale;
      if (param_delta > config_.max_param_change) {
        if (param_delta - param_delta != 0.0) {
          KALDI_WARN << "Infinite parameter change, will not apply.";
          SetZero(false, delta_nnet_);
        } else {
          scale *= config_.max_param_change / param_delta;
          KALDI_LOG << "Parameter change too big: " << param_delta << " > "
                    << "--max-param-change=" << config_.max_param_change
                    << ", scaling by " << config_.max_param_change / param_delta;
        }
      }
    }
    AddNnet(*delta_nnet_, scale, nnet_);
    ScaleNnet(config_.momentum, delta_nnet_);
  }
}

void NnetPerturbedTrainer::Train(const NnetExample &eg) {
  bool need_model_derivative = true;
  NnetExample eg_perturbed(eg);
  if (RandInt(0, 100) < config_.perturb_proportion * 100) {
    KALDI_LOG << "training with epsilon with " << config_.epsilon;
    ComputationRequest request;
    GetComputationRequest(*nnet_, eg, need_model_derivative,
                          config_.store_component_stats,
                          &request);
    const NnetComputation *computation = compiler_.Compile(request);
    Nnet nnet_temp(*nnet_);
    NnetComputer computer(config_.compute_config, *computation,
                          *nnet_,
                          &nnet_temp);
    // give the inputs to the computer object.
    computer.AcceptInputs(*nnet_, eg.io);
    computer.Forward();
  
    this->ProcessOutputs(eg, &computer);
    computer.Backward();
 
    int32 minibatch_size = 0;

    for (size_t i = 0; i < eg_perturbed.io.size(); i++) {
      NnetIo io = eg_perturbed.io[i];
      if (io.name == "ivector") {
        minibatch_size = io.features.NumRows();
      }
    }
    if (minibatch_size == 0) {
      KALDI_ERR << "Currently this experimental recipe only supports training with ivectors~~~";
    }
    
    CuVector<BaseFloat> deriv_norm(minibatch_size);
    std::vector<CuMatrix<BaseFloat> > input_derivs;
    for (size_t i = 0; i < eg_perturbed.io.size(); i++) {
      NnetIo io = eg_perturbed.io[i];
      int32 node_index = nnet_->GetNodeIndex(io.name);
      if (node_index == -1)
        KALDI_ERR << "No node named '" << io.name << "' in nnet.";
      // KALDI_LOG << "num rows: " << io.features.NumRows() << " num cols: " << io.features.NumCols();
      if (nnet_->IsInputNode(node_index)) {
        CuMatrix<BaseFloat> input_deriv(io.features.NumRows(),
                                     io.features.NumCols(),
                                     kUndefined);
        input_deriv = computer.GetInputDeriv(io.name);
        // KALDI_LOG << io.name << " input_deriv norm is " << input_deriv.FrobeniusNorm();
        // KALDI_LOG << io.name << " input_deriv is " << input_deriv.Range(0,10,0,10);
        input_derivs.push_back(input_deriv);

        if (io.name == "ivector") {
          for (int32 i = 0; i < minibatch_size; i++) {
            BaseFloat tmp = input_deriv.Row(i).Norm(2.0);
            deriv_norm(i) += tmp * tmp;
          }
        } else {
          int32 block_size = io.features.NumRows() / minibatch_size;
          for (int32 i = 0; i < minibatch_size; i++) {
            BaseFloat tmp = input_deriv.RowRange(i * block_size, block_size).FrobeniusNorm();
            deriv_norm(i) += tmp * tmp;
          }
        }
      }
    }
    deriv_norm.ApplyPow(0.5);
    if (deriv_norm.Norm(2.0) > 0) {
      for (size_t i = 0; i < eg_perturbed.io.size(); i++) {
        NnetIo io = eg_perturbed.io[i];
        int32 node_index = nnet_->GetNodeIndex(io.name);
        if (nnet_->IsInputNode(node_index)) {
          if (io.name == "ivector") {
            input_derivs[i].DivRowsVec(deriv_norm);
          } else {
            int32 block_size = io.features.NumRows() / minibatch_size;
            for (int32 j = 0; j < minibatch_size; j++) {
              input_derivs[i].RowRange(j * block_size, block_size).Scale(1 / deriv_norm(j));
            }
          }
          CuMatrix<BaseFloat> cu_input(io.features.NumRows(),
                                       io.features.NumCols(),
                                       kUndefined);
          cu_input.CopyFromGeneralMat(io.features);
          cu_input.AddMat(-config_.epsilon, input_derivs[i]);  
          Matrix<BaseFloat> input(cu_input);
          io.features = input;
          eg_perturbed.io[i] = io;
         // CuMatrix<BaseFloat> diff(io.features.NumRows(), io.features.NumCols(), kUndefined);
         // diff.CopyFromGeneralMat(io.features);
         // io2.features.AddToMat(-1.0, &diff, kNoTrans);
         // KALDI_LOG << diff.Range(0,10,0,10);
        }
      }
    }
  }
 // for (size_t i = 0; i < 2; i++) {
 //   NnetIo io = eg_perturbed.io[i];
 //   CuMatrix<BaseFloat> fe(io.features.NumRows(), io.features.NumCols(), kUndefined);
 //   fe.CopyFromGeneralMat(io.features);
 //   KALDI_LOG << "feature norm " << fe.FrobeniusNorm();
 // }
  {
    ComputationRequest request;
    GetComputationRequest(*nnet_, eg_perturbed, need_model_derivative,
                          config_.store_component_stats,
                          &request);
  
    const NnetComputation *computation = compiler_.Compile(request);
  
    NnetComputer computer(config_.compute_config, *computation,
                          *nnet_,
                          (delta_nnet_ == NULL ? nnet_ : delta_nnet_));
    // give the inputs to the computer object.
    computer.AcceptInputs(*nnet_, eg_perturbed.io);
    computer.Forward();
  
    this->ProcessOutputs2(eg_perturbed, &computer);
    computer.Backward();
    
    if (delta_nnet_ != NULL) {
      BaseFloat scale = (1.0 - config_.momentum);
      if (config_.max_param_change != 0.0) {
        BaseFloat param_delta =
            std::sqrt(DotProduct(*delta_nnet_, *delta_nnet_)) * scale;
        if (param_delta > config_.max_param_change) {
          if (param_delta - param_delta != 0.0) {
            KALDI_WARN << "Infinite parameter change, will not apply.";
            SetZero(false, delta_nnet_);
          } else {
            scale *= config_.max_param_change / param_delta;
            KALDI_LOG << "Parameter change too big: " << param_delta << " > "
                      << "--max-param-change=" << config_.max_param_change
                      << ", scaling by " << config_.max_param_change / param_delta;
          }
        }
      }
      AddNnet(*delta_nnet_, scale, nnet_);
      ScaleNnet(config_.momentum, delta_nnet_);
    }
  }
}

void NnetTrainer::ProcessOutputs(const NnetExample &eg,
                                 NnetComputer *computer) {
  std::vector<NnetIo>::const_iterator iter = eg.io.begin(),
      end = eg.io.end();
  for (; iter != end; ++iter) {
    const NnetIo &io = *iter;
    int32 node_index = nnet_->GetNodeIndex(io.name);
    KALDI_ASSERT(node_index >= 0);
    if (nnet_->IsOutputNode(node_index)) {
      ObjectiveType obj_type = nnet_->GetNode(node_index).u.objective_type;
      BaseFloat tot_weight, tot_objf;
      bool supply_deriv = true;
      ComputeObjectiveFunction(io.features, obj_type, io.name,
                               supply_deriv, computer,
                               &tot_weight, &tot_objf);
      objf_info_[io.name].UpdateStats(io.name, config_.print_interval,
                                      num_minibatches_processed_++,
                                      tot_weight, tot_objf);
    }
  }
}

void NnetTrainer::ProcessOutputs2(const NnetExample &eg,
                                 NnetComputer *computer) {
  std::vector<NnetIo>::const_iterator iter = eg.io.begin(),
      end = eg.io.end();
  for (; iter != end; ++iter) {
    const NnetIo &io = *iter;
    int32 node_index = nnet_->GetNodeIndex(io.name);
    KALDI_ASSERT(node_index >= 0);
    if (nnet_->IsOutputNode(node_index)) {
      ObjectiveType obj_type = nnet_->GetNode(node_index).u.objective_type;
      BaseFloat tot_weight, tot_objf;
      bool supply_deriv = true;
      ComputeObjectiveFunction(io.features, obj_type, io.name,
                               supply_deriv, computer,
                               &tot_weight, &tot_objf);
      objf_info2_[io.name].UpdateStats(io.name, config_.print_interval,
                                      num_minibatches_processed_++,
                                      tot_weight, tot_objf);
    }
  }
}

bool NnetTrainer::PrintTotalStats() const {
  unordered_map<std::string, ObjectiveFunctionInfo>::const_iterator
      iter = objf_info_.begin(),
      end = objf_info_.end();
  bool ans = false;
  for (; iter != end; ++iter) {
    const std::string &name = iter->first;
    const ObjectiveFunctionInfo &info = iter->second;
    ans = ans || info.PrintTotalStats(name);
  }
  return ans;
}

void ObjectiveFunctionInfo::UpdateStats(
    const std::string &output_name,
    int32 minibatches_per_phase,
    int32 minibatch_counter,
    BaseFloat this_minibatch_weight,
    BaseFloat this_minibatch_tot_objf) {
  int32 phase = minibatch_counter / minibatches_per_phase;
  if (phase != current_phase) {
    KALDI_ASSERT(phase == current_phase + 1); // or doesn't really make sense.
    PrintStatsForThisPhase(output_name, minibatches_per_phase);
    current_phase = phase;
    tot_weight_this_phase = 0.0;
    tot_objf_this_phase = 0.0;
  }
  tot_weight_this_phase += this_minibatch_weight;
  tot_objf_this_phase += this_minibatch_tot_objf;
  tot_weight += this_minibatch_weight;
  tot_objf += this_minibatch_tot_objf;
}

void ObjectiveFunctionInfo::PrintStatsForThisPhase(
    const std::string &output_name,
    int32 minibatches_per_phase) const {
  int32 start_minibatch = current_phase * minibatches_per_phase,
      end_minibatch = start_minibatch + minibatches_per_phase - 1;
  KALDI_LOG << "Average objective function for '" << output_name
            << "' for minibatches " << start_minibatch
            << '-' << end_minibatch << " is "
            << (tot_objf_this_phase / tot_weight_this_phase) << " over "
            << tot_weight_this_phase << " frames.";
}

bool ObjectiveFunctionInfo::PrintTotalStats(const std::string &name) const {
  KALDI_LOG << "Overall average objective function for '" << name << "' is "
            << (tot_objf / tot_weight) << " over " << tot_weight << " frames.";
  KALDI_LOG << "[this line is to be parsed by a script:] "
            << "log-prob-per-frame="
            << (tot_objf / tot_weight);
  return (tot_weight != 0.0);
}

NnetTrainer::~NnetTrainer() {
  delete delta_nnet_;
}

void ComputeObjectiveFunction(const GeneralMatrix &supervision,
                              ObjectiveType objective_type,
                              const std::string &output_name,
                              bool supply_deriv,
                              NnetComputer *computer,
                              BaseFloat *tot_weight,
                              BaseFloat *tot_objf) {
  const CuMatrixBase<BaseFloat> &output = computer->GetOutput(output_name);

  if (output.NumCols() != supervision.NumCols())
    KALDI_ERR << "Nnet versus example output dimension (num-classes) "
              << "mismatch for '" << output_name << "': " << output.NumCols()
              << " (nnet) vs. " << supervision.NumCols() << " (egs)\n";

  switch (objective_type) {
    case kLinear: {
      // objective is x * y.
      switch (supervision.Type()) {
        case kSparseMatrix: {
          const SparseMatrix<BaseFloat> &post = supervision.GetSparseMatrix();
          CuSparseMatrix<BaseFloat> cu_post(post);
          // The cross-entropy objective is computed by a simple dot product,
          // because after the LogSoftmaxLayer, the output is already in the form
          // of log-likelihoods that are normalized to sum to one.
          *tot_weight = cu_post.Sum();
          *tot_objf = TraceMatSmat(output, cu_post, kTrans);
          if (supply_deriv) {
            CuMatrix<BaseFloat> output_deriv(output.NumRows(), output.NumCols(),
                                             kUndefined);
            cu_post.CopyToMat(&output_deriv);
            computer->AcceptOutputDeriv(output_name, &output_deriv);
          }
          break;
        }
        case kFullMatrix: {
          // there is a redundant matrix copy in here if we're not using a GPU
          // but we don't anticipate this code branch being used in many cases.
          CuMatrix<BaseFloat> cu_post(supervision.GetFullMatrix());
          *tot_weight = cu_post.Sum();
          *tot_objf = TraceMatMat(output, cu_post, kTrans);
          if (supply_deriv)
            computer->AcceptOutputDeriv(output_name, &cu_post);
          break;
        }
        case kCompressedMatrix: {
          Matrix<BaseFloat> post;
          supervision.GetMatrix(&post);
          CuMatrix<BaseFloat> cu_post;
          cu_post.Swap(&post);
          *tot_weight = cu_post.Sum();
          *tot_objf = TraceMatMat(output, cu_post, kTrans);
          if (supply_deriv)
            computer->AcceptOutputDeriv(output_name, &cu_post);
          break;
        }
      }
      break;
    }
    case kQuadratic: {
      // objective is -0.5 (x - y)^2
      CuMatrix<BaseFloat> diff(supervision.NumRows(),
                               supervision.NumCols(),
                               kUndefined);
      diff.CopyFromGeneralMat(supervision);
      diff.AddMat(-1.0, output);
      *tot_weight = diff.NumRows();
      *tot_objf = -0.5 * TraceMatMat(diff, diff, kTrans);
      if (supply_deriv)
        computer->AcceptOutputDeriv(output_name, &diff);
      break;
    }
    default:
      KALDI_ERR << "Objective function type " << objective_type
                << " not handled.";
  }
}



} // namespace nnet3
} // namespace kaldi
