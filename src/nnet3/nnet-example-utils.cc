// nnet3/nnet-example-utils.cc

// Copyright 2012-2015    Johns Hopkins University (author: Daniel Povey)
//                2014    Vimal Manohar

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

#include "nnet3/nnet-example-utils.h"
#include "lat/lattice-functions.h"
#include "hmm/posterior.h"

namespace kaldi {
namespace nnet3 {


// get a sorted list of all NnetIo names in all examples in the list (will
// normally be just the strings "input" and "output", but maybe also "ivector").
static void GetIoNames(const std::vector<NnetExample> &src,
                            std::vector<std::string> *names_vec) {
  std::set<std::string> names;
  std::vector<NnetExample>::const_iterator iter = src.begin(), end = src.end();
  for (; iter != end; ++iter) {
    std::vector<NnetIo>::const_iterator iter2 = iter->io.begin(),
                                         end2 = iter->io.end();
    for (; iter2 != end2; ++iter2)
      names.insert(iter2->name);
  }
  CopySetToVector(names, names_vec);
}

// Get feature "sizes" for each NnetIo name, which are the total number of
// Indexes for that NnetIo (needed to correctly size the output matrix).  Also
// make sure the dimensions are consistent for each name.
static void GetIoSizes(const std::vector<NnetExample> &src,
                       const std::vector<std::string> &names,
                       std::vector<int32> *sizes) {
  std::vector<int32> dims(names.size(), -1);  // just for consistency checking.
  sizes->clear();
  sizes->resize(names.size(), 0);
  std::vector<std::string>::const_iterator names_begin = names.begin(),
                                             names_end = names.end();
  std::vector<NnetExample>::const_iterator iter = src.begin(), end = src.end();
  for (; iter != end; ++iter) {
    std::vector<NnetIo>::const_iterator iter2 = iter->io.begin(),
                                         end2 = iter->io.end();
    for (; iter2 != end2; ++iter2) {
      const NnetIo &io = *iter2;
      std::vector<std::string>::const_iterator names_iter =
          std::lower_bound(names_begin, names_end, io.name);
      KALDI_ASSERT(*names_iter == io.name);
      int32 i = names_iter - names_begin;
      int32 this_dim = io.features.NumCols();
      if (dims[i] == -1)
        dims[i] = this_dim;
      else if(dims[i] != this_dim) {
        KALDI_ERR << "Merging examples with inconsistent feature dims: "
                  << dims[i] << " vs. " << this_dim << " for '"
                  << io.name << "'.";
      }
      KALDI_ASSERT(io.features.NumRows() == io.indexes.size());
      int32 this_size = io.indexes.size();
      (*sizes)[i] += this_size;
    }
  }
}




// Do the final merging of NnetIo, once we have obtained the names, dims and
// sizes for each feature/supervision type.
static void MergeIo(const std::vector<NnetExample> &src,
                    const std::vector<std::string> &names,
                    const std::vector<int32> &sizes,
                    bool compress,
                    NnetExample *merged_eg) {
  int32 num_feats = names.size();
  std::vector<int32> cur_size(num_feats, 0);
  std::vector<std::vector<GeneralMatrix const*> > output_lists(num_feats);
  merged_eg->io.clear();
  merged_eg->io.resize(num_feats);
  for (int32 f = 0; f < num_feats; f++) {
    NnetIo &io = merged_eg->io[f];
    int32 size = sizes[f];
    KALDI_ASSERT(size > 0);
    io.name = names[f];
    io.indexes.resize(size);
  }

  std::vector<std::string>::const_iterator names_begin = names.begin(),
                                             names_end = names.end();
  std::vector<NnetExample>::const_iterator iter = src.begin(), end = src.end();
  for (int32 n = 0; iter != end; ++iter,++n) {
    std::vector<NnetIo>::const_iterator iter2 = iter->io.begin(),
                                         end2 = iter->io.end();
    for (; iter2 != end2; ++iter2) {
      const NnetIo &io = *iter2;
      std::vector<std::string>::const_iterator names_iter =
          std::lower_bound(names_begin, names_end, io.name);
      KALDI_ASSERT(*names_iter == io.name);
      int32 f = names_iter - names_begin;
      int32 this_size = io.indexes.size(),
        &this_offset = cur_size[f];
      KALDI_ASSERT(this_size + this_offset <= sizes[f]);
      output_lists[f].push_back(&(io.features));
      NnetIo &output_io = merged_eg->io[f];
      std::copy(io.indexes.begin(), io.indexes.end(),
                output_io.indexes.begin() + this_offset);
      std::vector<Index>::iterator output_iter = output_io.indexes.begin();
      // Set the n index to be different for each of the original examples.
      for (int32 i = this_offset; i < this_offset + this_size; i++) {
        // we could easily support merging already-merged egs, but I don't see a
        // need for it right now.
        KALDI_ASSERT(output_iter[i].n == 0 &&
                     "Merging already-merged egs?  Not currentlysupported.");
        output_iter[i].n = n;
      }
      this_offset += this_size;  // note: this_offset is a reference.
    }
  }
  KALDI_ASSERT(cur_size == sizes);
  for (int32 f = 0; f < num_feats; f++) {
    AppendGeneralMatrixRows(output_lists[f],
                            &(merged_eg->io[f].features));
    if (compress) {
      // the following won't do anything if the features were sparse.
      merged_eg->io[f].features.Compress();
    }
  }
}



void MergeExamples(const std::vector<NnetExample> &src,
                   bool compress,
                   NnetExample *merged_eg) {
  KALDI_ASSERT(!src.empty());
  std::vector<std::string> io_names;
  GetIoNames(src, &io_names);
  // the sizes are the total number of Indexes we have across all examples.
  std::vector<int32> io_sizes;
  GetIoSizes(src, io_names, &io_sizes);
  MergeIo(src, io_names, io_sizes, compress, merged_eg);
}


void GetComputationRequest(const Nnet &nnet,
                           const NnetExample &eg,
                           bool need_model_derivative,
                           bool store_component_stats,
                           ComputationRequest *request) {
  request->inputs.clear();
  request->inputs.reserve(eg.io.size());
  request->outputs.clear();
  request->outputs.reserve(eg.io.size());
  request->need_model_derivative = need_model_derivative;
  request->store_component_stats = store_component_stats;
  for (size_t i = 0; i < eg.io.size(); i++) {
    const NnetIo &io = eg.io[i];
    const std::string &name = io.name;
    int32 node_index = nnet.GetNodeIndex(name);
    if (node_index == -1 &&
        !nnet.IsInputNode(node_index) && !nnet.IsOutputNode(node_index))
      KALDI_ERR << "Nnet example has input or output named '" << name
                << "', but no such input or output node is in the network.";

    std::vector<IoSpecification> &dest =
        nnet.IsInputNode(node_index) ? request->inputs : request->outputs;
    dest.resize(dest.size() + 1);
    IoSpecification &io_spec = dest.back();
    io_spec.name = name;
    io_spec.indexes = io.indexes;
    io_spec.has_deriv = need_model_derivative;
    // io_spec.has_deriv = nnet.IsOutputNode(node_index) && need_model_derivative;
  }
  // check to see if something went wrong.
  if (request->inputs.empty())
    KALDI_ERR << "No inputs in computation request.";
  if (request->outputs.empty())
    KALDI_ERR << "No outputs in computation request.";
}

} // namespace nnet3
} // namespace kaldi
