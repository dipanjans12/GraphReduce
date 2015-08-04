#!/bin/bash

number=0
while [ $number -lt 5 ]; do
      echo "Number = $number"
      ./sssp ../datasets/datasets/uk-2002.mtx 0
      #./sssp ../datasets/datasets/kron_g500-logn21.mtx 0
      #/sssp ../datasets/datasets/indochina-2004.mtx 0
      #./sssp ../datasets/datasets/nlpkkt160.mtx 0
      #./sssp ../datasets/datasets/soc-LiveJournal1.mtx 0
      number=$((number + 1))
done
