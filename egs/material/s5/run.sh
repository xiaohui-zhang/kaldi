#!/bin/bash
# Copyright (c) 2017, Johns Hopkins University (Jan "Yenda" Trmal<jtrmal@gmail.com>)
# License: Apache 2.0

# Begin configuration section.
# End configuration section
. ./path.sh
. ./cmd.sh

nj=30 # number of parallel jobs
stage=1
language=swahili
. utils/parse_options.sh

set -e -o pipefail
set -o nounset                              # Treat unset variables as an error

[ ! -f ./conf/lang/${language}.conf ] && echo "Language configuration conf/lang/${language}.conf does not exist!" && exit 1
. ./conf/lang/${language}.conf

if [ $stage -le 1 ]; then
  local/prepare_text_data.sh $corpus $language
  local/prepare_audio_data.sh $corpus $language
fi

if [ $stage -le 2 ]; then
  local/prepare_dict.sh $corpus $language
  utils/validate_dict_dir.pl data/$language/local/dict_nosp
  utils/prepare_lang.sh data/$language/local/dict_nosp \
    "<unk>" data/$language/local/lang_nosp data/$language/lang_nosp
  utils/validate_lang.pl data/$language/lang_nosp
fi

if [ $stage -le 3 ]; then
  local/train_lms_srilm.sh --oov-symbol "<unk>" --words-file \
    data/$language/lang_nosp/words.txt data/$language data/$language/lm
  utils/format_lm.sh data/$language/lang_nosp data/$language/lm/lm.gz \
    data/$language/local/dict_nosp/lexiconp.txt data/$language/lang_nosp_test
  utils/validate_lang.pl data/$language/lang_nosp_test
fi

if [ $stage -le 4 ]; then
  for set in train dev; do
    dir=data/$language/$set
    utils/fix_data_dir.sh $dir
    steps/make_mfcc.sh --cmd "$train_cmd" --nj 16 $dir
    steps/compute_cmvn_stats.sh $dir
    utils/fix_data_dir.sh $dir
    utils/validate_data_dir.sh $dir
  done
fi

# Create a subset with 40k short segments to make flat-start training easier
if [ $stage -le 5 ]; then
  utils/subset_data_dir.sh --shortest data/$language/train $numShorestUtts data/$language/train_short
fi

# monophone training
if [ $stage -le 6 ]; then
  steps/train_mono.sh --nj $nj --cmd "$train_cmd" \
    data/$language/train_short data/$language/lang_nosp_test exp/$language/mono
  (
    utils/mkgraph.sh data/$language/lang_nosp_test \
      exp/$language/mono exp/$language/mono/graph_nosp
    for test in dev; do
      steps/decode.sh --nj $nj --cmd "$decode_cmd" exp/$language/mono/graph_nosp \
        data/$language/$test exp/$language/mono/decode_nosp_$test
    done
  )&

  steps/align_si.sh --nj $nj --cmd "$train_cmd" \
    data/$language/train data/$language/lang_nosp_test exp/$language/mono exp/$language/mono_ali
fi

# train a first delta + delta-delta triphone system on all utterances
if [ $stage -le 7 ]; then
  steps/train_deltas.sh --cmd "$train_cmd" \
    $numLeavesTri1 $numGaussTri1 data/$language/train data/$language/lang_nosp_test exp/$language/mono_ali exp/$language/tri1

  # decode using the tri1 model
  (
    utils/mkgraph.sh data/$language/lang_nosp_test exp/$language/tri1 exp/$language/tri1/graph_nosp
    for test in dev; do
      steps/decode.sh --nj $nj --cmd "$decode_cmd" exp/$language/tri1/graph_nosp \
        data/$language/$test exp/$language/tri1/decode_nosp_$test
    done
  )&

  steps/align_si.sh --nj $nj --cmd "$train_cmd" \
    data/$language/train data/$language/lang_nosp_test exp/$language/tri1 exp/$language/tri1_ali
fi

# train an LDA+MLLT system.
if [ $stage -le 8 ]; then
  steps/train_lda_mllt.sh --cmd "$train_cmd" \
    --splice-opts "--left-context=3 --right-context=3" $numLeavesTri2 $numGaussTri2 \
    data/$language/train data/$language/lang_nosp_test exp/$language/tri1_ali exp/$language/tri2

  # decode using the LDA+MLLT model
  (
    utils/mkgraph.sh data/$language/lang_nosp_test exp/$language/tri2 exp/$language/tri2/graph_nosp
    for test in dev; do
      steps/decode.sh --nj $nj --cmd "$decode_cmd" exp/$language/tri2/graph_nosp \
        data/$language/$test exp/$language/tri2/decode_nosp_$test
    done
  )&

  steps/align_si.sh  --nj $nj --cmd "$train_cmd" --use-graphs true \
    data/$language/train data/$language/lang_nosp_test exp/$language/tri2 exp/$language/tri2_ali
fi

# Train tri3, which is LDA+MLLT+SAT
if [ $stage -le 9 ]; then
  steps/train_sat.sh --cmd "$train_cmd" $numLeavesTri3 $numGaussTri3 \
    data/$language/train data/$language/lang_nosp_test exp/$language/tri2_ali exp/$language/tri3

  # decode using the tri3 model
  (
    utils/mkgraph.sh data/$language/lang_nosp_test exp/$language/tri3 exp/$language/tri3/graph_nosp
    for test in dev; do
      steps/decode_fmllr.sh --nj $nj --cmd "$decode_cmd" exp/$language/tri3/graph_nosp \
        data/$language/$test exp/$language/tri3/decode_nosp_$test
    done
  )&
fi

# Now we compute the pronunciation and silence probabilities from training data,
# and re-create the lang directory.
if [ $stage -le 10 ]; then
  steps/get_prons.sh --cmd "$train_cmd" data/$language/train data/$language/lang_nosp_test exp/$language/tri3
  utils/dict_dir_add_pronprobs.sh --max-normalize true \
    data/$language/local/dict_nosp \
    exp/$language/tri3/pron_counts_nowb.txt exp/$language/tri3/sil_counts_nowb.txt \
    exp/$language/tri3/pron_bigram_counts_nowb.txt data/$language/local/dict

  utils/prepare_lang.sh data/$language/local/dict "<unk>" data/$language/local/lang data/$language/lang

  utils/format_lm.sh data/$language/lang data/$language/lm/lm.gz \
    data/$language/local/dict/lexiconp.txt data/$language/lang_test

  steps/align_fmllr.sh --nj $nj --cmd "$train_cmd" \
    data/$language/train data/$language/lang_test exp/$language/tri3 exp/$language/tri3_ali
fi

if [ $stage -le 11 ]; then
  # Test the tri3 system with the silprobs and pron-probs.

  # decode using the tri3 model
  utils/mkgraph.sh data/$language/lang_test exp/$language/tri3 exp/$language/tri3/graph
  for test in dev; do
    steps/decode_fmllr.sh --nj $nj --cmd "$decode_cmd" \
      exp/$language/tri3/graph data/$language/$test exp/$language/tri3/decode_$test
  done
fi
