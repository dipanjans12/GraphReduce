rm -rf out
nvprof ./bfs ../datasets/datasets/kron_g500-logn21.mtx 0 >>out 2>>out
nvprof ./bfs ../datasets/datasets/indochina-2004.mtx 0  >>out 2>>out
nvprof ./bfs ../datasets/datasets/uk-2002.mtx 0  >>out 2>>out
nvprof ./bfs ../datasets/datasets/orkut.mtx 0  >>out 2>>out
nvprof ./bfs ../datasets/datasets/cage15.mtx 0  >>out 2>>out

nvprof ./pagerank ../datasets/datasets/kron_g500-logn21.mtx  >>out 2>>out
nvprof ./pagerank ../datasets/datasets/indochina-2004.mtx  >>out 2>>out
nvprof ./pagerank ../datasets/datasets/uk-2002.mtx  >>out 2>>out
nvprof ./pagerank ../datasets/datasets/orkut.mtx  >>out 2>>out
nvprof ./pagerank ../datasets/datasets/cage15.mtx  >>out 2>>out

nvprof ./connected_component ../datasets/datasets/kron_g500-logn21.mtx  >>out 2>>out
nvprof ./connected_component ../datasets/datasets/indochina-2004.mtx  >>out 2>>out
nvprof ./connected_component ../datasets/datasets/uk-2002.mtx  >>out 2>>out
nvprof ./connected_component ../datasets/datasets/orkut.mtx  >>out 2>>out
nvprof ./connected_component ../datasets/datasets/cage15.mtx  >>out 2>>out

 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
