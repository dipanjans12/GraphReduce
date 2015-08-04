#!/bin/bash

number=0
while [ $number -lt 5 ]; do
      echo "Number = $number"
      #./bfs ../datasets/datasets/uk-2002.mtx 0
      #./bfs ../datasets/datasets/kron_g500-logn21.mtx 0
      #./bfs ../datasets/datasets/indochina-2004.mtx 0
      #./bfs ../datasets/datasets/nlpkkt160.mtx 0
      #./bfs ../datasets/datasets/soc-LiveJournal1.mtx 0
      #./bfs ../datasets/datasets/delaunay_n24.mtx 0
      #./bfs ../datasets/datasets/rgg_n_2_24_s0.mtx 0
      number=$((number + 1))
done
number=0
while [ $number -lt 5 ]; do
      echo "Number = $number"
      #./bfs ../datasets/datasets/uk-2002.mtx 0
      #./bfs ../datasets/datasets/kron_g500-logn21.mtx 0
      #./bfs ../datasets/datasets/indochina-2004.mtx 0
      #./bfs ../datasets/datasets/nlpkkt160.mtx 0
      #./bfs ../datasets/datasets/soc-LiveJournal1.mtx 0
      #./bfs ../datasets/datasets/delaunay_n24.mtx 0
      #./pagerank ../datasets/datasets/rgg_n_2_24_s0.mtx 
      number=$((number + 1))
done
number=0
while [ $number -lt 5 ]; do
      echo "Number = $number"
      #./bfs ../datasets/datasets/uk-2002.mtx 0
      #./bfs ../datasets/datasets/kron_g500-logn21.mtx 0
      #./bfs ../datasets/datasets/indochina-2004.mtx 0
      #./bfs ../datasets/datasets/nlpkkt160.mtx 0
      #./bfs ../datasets/datasets/soc-LiveJournal1.mtx 0
      #./bfs ../datasets/datasets/delaunay_n24.mtx 0
      ./connected_component ../datasets/datasets/rgg_n_2_24_s0.mtx 
      number=$((number + 1))
done
