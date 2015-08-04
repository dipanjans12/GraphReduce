#!/bin/bash

number=0
while [ $number -lt 5 ]; do
      echo "Number = $number"
      #./connected_component ../datasets/datasets/uk-2002.mtx 
      #./connected_component ../datasets/datasets/kron_g500-logn21.mtx 
      #./connected_component ../datasets/datasets/indochina-2004.mtx 
      #./connected_component ../datasets/datasets/nlpkkt160.mtx
      #./connected_component ../datasets/datasets/soc-LiveJournal1.mtx
      ./connected_component ../datasets/datasets/delaunay_n24.mtx
      number=$((number + 1))
done
