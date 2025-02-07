#!/bin/bash

declare -A named_args=(
  ["git_token_name"]="fuzz-docker"
  ["git_token"]=""
  #["git_branch"]="fuzz-fw-update"
  ["git_branch"]="develop"
  ["fuzz_time_sec"]=100
  ["build_image"]=0
  ["run_cli"]=0
)

# Function to display usage information
function show_usage() {
  echo "Usage: $0 [--git_token_name value] [--git_token value] [--git_branch value] [--fuzz_time_sec value] [--run_cli] [--build_image] [--help] command"
  exit 1
}

# Parse the arguments and update the associative array with user-provided values
while [[ $# -gt 0 ]]; do
  case "$1" in
    --git_token_name)
      named_args["git_token_name"]=$2
      shift 2
      ;;
    --git_token)
      named_args["git_token"]=$2
      shift 2
      ;;
    --git_branch)
      named_args["git_branch"]=$2
      shift 2
      ;;
    --fuzz_time_sec)
      named_args["fuzz_time_sec"]=$2
      shift 2
      ;;
    --run_cli)
      named_args["run_cli"]=1
      shift
      ;;
    --build_image)
      named_args["build_image"]=1
      shift
      ;;
    --help)
      show_usage
      exit 0
      ;;
    *)
      command=$1
      shift
      ;;
  esac
done

build_image=${named_args["build_image"]}
if [ "$build_image" -eq 1 ]; then
  rm -rf AFLplusplus
  git clone https://github.com/AFLplusplus/AFLplusplus

  cp -f Dockerfile AFLplusplus/Dockerfile
  cd AFLplusplus

  sudo docker build --build-arg git_token_name=${named_args["git_token_name"]} \
    --build-arg git_token=${named_args["git_token"]} \
    --build-arg git_branch=${named_args["git_branch"]} \
    -t afl:pldm_fuzz_mod .
  
  cd ..
fi

if [ "${named_args["run_cli"]}" -eq 1 ]; then
  echo "launching console ..."
  sudo docker run -it -v $(pwd):/src afl:pldm_fuzz_mod /bin/bash
else
  cd AFLplusplus

  if [ -z "$command" ]; then
      sudo docker run afl:pldm_fuzz_mod /pldm/builddir/test/fuzz/pldmd_mockup -h
  else
      cd AFLplusplus
      aflOutName=aflOut$(date +%s)
      mkdir ${aflOutName}
      sudo docker run -v "$(pwd):/src" -v "$(pwd)/../${aflOutName}:/out" afl:pldm_fuzz_mod /bin/bash -c "service dbus restart; afl-fuzz -o /out -i ./seeds/ -V ${named_args["fuzz_time_sec"]} -- ./pldmd_mockup ${command}"
      cd ..
  fi
fi

