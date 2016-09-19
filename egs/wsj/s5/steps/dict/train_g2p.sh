#!/bin/bash
# Copyright 2014  Johns Hopkins University (Author: Yenda Trmal)
# Copyright 2017  Xiaohui Zhang
# Apache 2.0

# Begin configuration section.  
iters=7
stage=0
only_words=true
cmd=run.pl
# a list of silence phones, like data/local/dict/silence_phones.txt
silence_phones=
# End configuration section.

echo "$0 $@"  # Print the command line for logging

[ -f ./path.sh ] && . ./path.sh; # source the path.
. utils/parse_options.sh || exit 1;
set -u
set -e

if [ $# != 2 ]; then
   echo "Usage: $0 [options] <lexicon-in> <work-dir>"
   echo "    where <lexicon-in> is the training lexicon (one pronunciation per "
   echo "    word per line) and <word-dir> is directory where the models will "
   echo "    be stored"
   echo "e.g.: train_g2p.sh data/local/lexicon.txt exp/g2p/"
   echo ""
   echo "main options (for others, see top of script file)"
   echo "  --iters <int>                                    # How many iterations. Relates to N-ngram order"
   echo "  --cmd (utils/run.pl|utils/queue.pl <queue opts>) # how to run jobs."
   exit 1;
fi

lexicon=$1
wdir=$2


mkdir -p $wdir/log

[ ! -f $lexicon ] && echo "$0: Training lexicon does not exist." && exit 1

# For input lexicon, remove pronunciations containing non-utf-8-encodable characters,
# and optionally remove words that are mapped to a single silence phone from the lexicon.
if [ $stage -le 0 ]; then
  if $only_words && [ ! -z "$silence_phones" ]; then
    awk 'NR==FNR{a[$1] = 1; next} {s=$2;for(i=3;i<=NF;i++) s=s" "$i; if(!(s in a)) print $1" "s}' \
      $silence_phones $lexicon | \
      awk '{printf("%s\t",$1); for (i=2;i<NF;i++){printf("%s ",$i);} printf("%s\n",$NF);}' | \
      iconv -c -t utf-8 -  | awk 'NF > 0'> $wdir/lexicon_tab_separated.txt
  else
    awk '{printf("%s\t",$1); for (i=2;i<NF;i++){printf("%s ",$i);} printf("%s\n",$NF);}' $lexicon | \
      iconv -c -t utf-8 -  | awk 'NF > 0'> $wdir/lexicon_tab_separated.txt
  fi
fi

if ! phonetisaurus=`which phonetisaurus-align` ; then
  echo "Phonetisarus was not found !"
  echo "Go to $KALDI_ROOT/tools and execute extras/install_phonetisaurus.sh"
  exit 1
fi

if [ $stage -le 0 ]; then
  # Align lexicon stage. Lexicon is assumed to have first column tab separated
  $cmd $wdir/log/g2p_align.0.log \
    phonetisaurus-align --input=$wdir/lexicon_tab_separated.txt --ofile=${wdir}/aligned_lexicon.corpus || exit 1;
fi

if [ $stage -le 1 ]; then
  # Train the n-gram model using srilm.
  $cmd $wdir/log/train_ngram.log \
    ngram-count -order $iters -kn-modify-counts-at-end -gt1min 0 -gt2min 0 \
    -gt3min 0 -gt4min 0 -gt5min 0 -gt6min 0 -gt7min 0 -ukndiscount \
    -text ${wdir}/aligned_lexicon.corpus -lm ${wdir}/aligned_lexicon.arpa
fi

if [ $stage -le 2 ]; then
  # Convert the arpa file to FST.
  $cmd $wdir/log/convert_fst.log \
    phonetisaurus-arpa2wfst --lm=${wdir}/aligned_lexicon.arpa --ofile=${wdir}/model.fst
fi
