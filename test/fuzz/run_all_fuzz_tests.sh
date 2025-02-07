#!/bin/bash

if [[ $1 = "" ]];
then
  echo 'Usage: run_all_fuzz_tests.sh [build_dir_path]'
  exit 0
fi

REQUIRED_PKG="tmux"
PKG_OK=$(dpkg-query -W --showformat='${Status}\n' $REQUIRED_PKG|grep "install ok installed")
if [ "" = "$PKG_OK" ]; then
  echo "No $REQUIRED_PKG. Setting up $REQUIRED_PKG."
  apt-get --yes install $REQUIRED_PKG
fi

session="pldm-fuzz-tests"
build_dir=$1
fuzz_test_path=${build_dir}"/test/fuzz/*"
seeds_path=${build_dir}"/test/fuzz/seeds/"

tmux new-session -d -s $session || true

i=1
for filename in $fuzz_test_path; do
  file_type=$(file $filename)

  if [[ "$file_type" == *"ELF 64-bit"* ]]; then
    window=$i
    command="afl-fuzz -o ${filename}_out -i ${seeds_path} -- ${filename} --fRespCapabilities 50 --alterHeader 50"
    echo "RUNNING: "$command
    
    window_name=$(basename $filename)
    
    tmux new-window -t $session:$window -n $window_name
    tmux send-keys -t $session:$window "$command" C-m

    ((i=i+1))
  fi
done
