#!/bin/bash

number=0
while [ $number -lt 5 ]; do
      echo "Number = $number"
      #./pagerank ../datasets/datasets/uk-2002.mtx 
      #./pagerank ../datasets/datasets/kron_g500-logn21.mtx 
      #./pagerank ../datasets/datasets/indochina-2004.mtx 
      #./pagerank ../datasets/datasets/nlpkkt160.mtx
      #./pagerank ../datasets/datasets/soc-LiveJournal1.mtx
      ./pagerank ../datasets/datasets/delaunay_n24.mtx
      number=$((number + 1))
done
