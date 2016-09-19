#!/bin/bash
# Copyright 2014  Johns Hopkins University (Author: Yenda Trmal)
# Copyright 2017  Xiaohui Zhang
# Apache 2.0

# Begin configuration section.  
stage=0
nbest=3  # Generate upto N pronunciation variants for each word 
# (The maximum size of the nbest list, not considering pruning and taking the prob-mass yet).
thresh=10 # Pruning threshold for n-best. A large threshold makes the nbest list 
# shorter, and less likely to hit the max size. 
pmass=0.9  # Select the top variants from the pruned nbest list, 
# summing up to this total prob-mass for a word.
# On the "boundary", it's greedy by design, e.g. if pmass = 0.8,
# and we have prob(pron_1) = 0.5, and prob(pron_2) = 0.4, then we get both.

cmd=run.pl
# End configuration section.

echo "$0 $@"  # Print the command line for logging

[ -f ./path.sh ] && . ./path.sh; # source the path.
. parse_options.sh || exit 1;

set -u
set -e

if [ $# != 3 ]; then
   echo "Usage: $0 [options] <word-list> <g2p-model-dir> <output-dir>"
   echo "... where <word-list> is a list of words whose pronunciation is to be generated"
   echo "          <g2p-model-dir> is a directory used as a target during training of G2P"
   echo "          <output-dir> is the directory where the output lexicon should be stored"
   echo "e.g.: $0 oov_words exp/g2p exp/g2p/oov_lex"
   echo ""
   echo "main options (for others, see top of script file)"
   echo "  --nbest <int>                                    # Generate upto N pronunciation variants for each word"
   echo "  --thresh <int>                                   # Pruning threshold for n-best."
   echo "  --pmass <float>                                  # Select the top variants from the pruned nbest list,"
   echo "                                                   # summing up to this total prob-mass for a word."
   echo "  --cmd (utils/run.pl|utils/queue.pl <queue opts>) # how to run jobs."
   exit 1;
fi

wordlist=$1
modeldir=$2
output=$3

mkdir -p $output/log

model=$modeldir/model.fst
[ ! -f ${model:-} ] && echo "File $model not found in the directory $modeldir." && exit 1
[ ! -f $wordlist ] && echo "File $wordlist not found!" && exit 1

cp $wordlist $output/wordlist.txt

if ! phonetisaurus=`which phonetisaurus-apply` ; then
  echo "Phonetisarus was not found !"
  echo "Go to $KALDI_ROOT/tools and execute extras/install_phonetisaurus.sh"
  exit 1
fi

echo "Applying the G2P model to wordlist $wordlist"

$cmd $output/log/apply_g2p.log \
  phonetisaurus-apply  --model $model --nbest $nbest --thresh $thresh \
    --accumulate --pmass $pmass --probs --verbose --word_list \
    "$output/wordlist.txt" '>' $output/lexicon.lex

# Remap the words from output file back to the original casing
# Conversion of some of thems might have failed, so we have to be careful
# and use the transform_map file we generated beforehand
# Also, because the sequitur output is not readily usable as lexicon (it adds 
# one more column with ordering of the pron. variants) convert it into the proper lexicon form
output_lex=$output/lexicon.lex

# Some words might have been removed or skipped during the process,
# let's check it and warn the user if so...
nlex=`cut -f 1 $output_lex | sort -u | wc -l`
nwlist=`cut -f 1 $output/wordlist.txt | sort -u | wc -l`
if [ $nlex -ne $nwlist ] ; then
  echo "WARNING: Unable to generate pronunciation for all words. ";
  echo "WARINNG:   Wordlist: $nwlist words"
  echo "WARNING:   Lexicon : $nlex words"
  echo "WARNING:Diff example: "
  diff <(cut -f 1 $output_lex | sort -u ) \
       <(cut -f 1 $output/wordlist.txt | sort -u ) || true
fi
exit 0
