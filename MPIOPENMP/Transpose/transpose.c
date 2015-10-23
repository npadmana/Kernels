/*
Copyright (c) 2013, Intel Corporation

Redistribution and use in source and binary forms, with or without 
modification, are permitted provided that the following conditions 
are met:

* Redistributions of source code must retain the above copyright 
      notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above 
      copyright notice, this list of conditions and the following 
      disclaimer in the documentation and/or other materials provided 
      with the distribution.
* Neither the name of Intel Corporation nor the names of its 
      contributors may be used to endorse or promote products 
      derived from this software without specific prior written 
      permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE 
COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, 
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER 
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN 
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
POSSIBILITY OF SUCH DAMAGE.
*/

/*******************************************************************

NAME:    transpose

PURPOSE: This program tests the efficiency with which a square matrix
         can be transposed and stored in another matrix. The matrices
         are distributed identically.
  
USAGE:   Program inputs are the matrix order, the number of times to 
         repeat the operation, and the communication mode

         transpose <#threads> <# iterations> <matrix size> [tile size]

         An optional parameter specifies the tile size used to divide the 
         individual matrix blocks for improved cache and TLB performance. 
  
         The output consists of diagnostics to make sure the 
         transpose worked and timing statistics.

FUNCTIONS CALLED:

         Other than MPI or standard C functions, the following 
         functions are used in this program:

          wtime()           Portable wall-timer interface.
          bail_out()        Determine global error and exit if nonzero.

HISTORY: Written by Tim Mattson, April 1999.  
         Updated by Rob Van der Wijngaart, December 2005.
         Updated by Rob Van der Wijngaart, October 2006.
         Updated by Rob Van der Wijngaart, November 2014::
         - made variable names more consistent 
         - put timing around entire iterative loop of transposes
         - fixed incorrect matrix block access; no separate function
           for local transpose of matrix block
         - reordered initialization and verification loops to
           produce unit stride
         - changed initialization values, such that the input matrix
           elements are: A(i,j) = i+order*j
         
  
*******************************************************************/

/******************************************************************
                     Layout nomenclature                         
                     -------------------

o Each rank owns one block of columns (Colblock) of the overall
  matrix to be transposed, as well as of the transposed matrix.
o Colblock is stored contiguously in the memory of the rank. 
  The stored format is column major, which means that matrix
  elements (i,j) and (i+1,j) are adjacent, and (i,j) and (i,j+1)
  are "order" words apart
o Colblock is logically composed of #ranks Blocks, but a Block is
  not stored contiguously in memory. Conceptually, the Block is 
  the unit of data that gets communicated between ranks. Block i of 
  rank j is locally transposed and gathered into a buffer called Work, 
  which is sent to rank i, where it is scattered into Block j of the 
  transposed matrix.
o When tiling is applied to reduce TLB misses, each block gets 
  accessed by tiles. 
o The original and transposed matrices are called A and B

 -----------------------------------------------------------------
|           |           |           |                             |
| Colblock  |           |           |                             |
|           |           |           |                             |
|           |           |           |                             |
|           |           |           |                             |
|        -------------------------------                          |
|           |           |           |                             |
|           |  Block    |           |                             |
|           |           |           |                             |
|           |           |           |                             |
|           |           |           |                             |
|        -------------------------------                          |
|           |Tile|      |           |                             |
|           |    |      |           |   Overall Matrix            |
|           |----       |           |                             |
|           |           |           |                             |
|           |           |           |                             |
|        -------------------------------                          |
|           |           |           |                             |
|           |           |           |                             |
|           |           |           |                             |
|           |           |           |                             |
|           |           |           |                             |
 -----------------------------------------------------------------*/

#include <par-res-kern_general.h>
#include <par-res-kern_mpiomp.h>

#define A(i,j)        A_p[(i+istart)+order*(j)]
#define B(i,j)        B_p[(i+istart)+order*(j)]
#define Work_in(i,j)  Work_in_p[i+Block_order*(j)]
#define Work_out(i,j) Work_out_p[i+Block_order*(j)]

int main(int argc, char ** argv)
{
  size_t Block_order;         /* number of columns owned by rank       */
  size_t Block_size;          /* size of a single block                */
  size_t Colblock_size;       /* size of column block                  */
  size_t Tile_order=32;       /* default Tile order                    */
  size_t tiling;              /* boolean: true if tiling is used       */
  int Num_procs;           /* number of ranks                       */
  size_t order;               /* order of overall matrix               */
  int send_to, recv_from;  /* ranks with which to communicate       */
  MPI_Status status;       
#ifndef SYNCHRONOUS
  MPI_Request send_req;
  MPI_Request recv_req;
#endif
  size_t bytes;              /* combined size of matrices             */
  int my_ID;               /* rank                                  */
  int root=0;              /* ID of root rank                       */
  size_t iterations;          /* number of times to do the transpose   */
  size_t i, j, it, jt, istart;/* dummies                               */
  size_t iter;                /* index of iteration                    */
  size_t phase;               /* phase inside staged communication     */
  size_t colstart;            /* starting column for owning rank       */
  int nthread_input,       /* thread parameters                     */
      nthread; 
  int error;               /* error flag                            */
  int concurrency;         /* number of threads that can be active  */
  double *A_p;             /* original matrix column block          */
  double *B_p;             /* transposed matrix column block        */
  double *Work_in_p;       /* workspace for the transpose function  */
  double *Work_out_p;      /* workspace for the transpose function  */
  double abserr,           /* absolute error                        */
         abserr_tot;       /* aggregate absolute error              */
  double epsilon = 1.e-8;  /* error tolerance                       */
  double local_trans_time, /* timing parameters                     */
         trans_time,
         avgtime;

/*********************************************************************
** Initialize the MPI environment
*********************************************************************/
  MPI_Init(&argc,&argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &my_ID);
  MPI_Comm_size(MPI_COMM_WORLD, &Num_procs);

/*********************************************************************
** process, test and broadcast input parameters
*********************************************************************/
  error = 0;
  if (my_ID == root) {
    if (argc != 4 && argc != 5){
      printf("Usage: %s <#threads><#iterations> <matrix order> [Tile size]\n",
                                                               *argv);
      error = 1; goto ENDOFTESTS;
    }

    /* Take number of threads to request from command line */
    nthread_input = atoi(*++argv); 
    if ((nthread_input < 1) || (nthread_input > MAX_THREADS)) {
      printf("ERROR: Invalid number of threads: %d\n", nthread_input);
      error = 1; 
      goto ENDOFTESTS; 
    }

    iterations  = atoi(*++argv);
    if(iterations < 1){
      printf("ERROR: iterations must be >= 1 : %zu \n",iterations);
      error = 1; goto ENDOFTESTS;
    }

    order = atoi(*++argv);
    if (order < Num_procs) {
      printf("ERROR: matrix order %zu should at least # procs %d\n", 
             order, Num_procs);
      error = 1; goto ENDOFTESTS;
    }
    if (order%Num_procs) {
      printf("ERROR: matrix order %zu should be divisible by # procs %d\n",
             order, Num_procs);
      error = 1; goto ENDOFTESTS;
    }

    if (argc == 5) Tile_order = atoi(*++argv);

    ENDOFTESTS:;
  }
  bail_out(error);
  /*  Broadcast input data to all ranks */
  MPI_Bcast(&order,         1, MPI_AINT, root, MPI_COMM_WORLD);
  MPI_Bcast(&iterations,    1, MPI_AINT, root, MPI_COMM_WORLD);
  MPI_Bcast(&Tile_order,    1, MPI_AINT, root, MPI_COMM_WORLD);
  MPI_Bcast(&nthread_input, 1, MPI_AINT, root, MPI_COMM_WORLD);

  omp_set_num_threads(nthread_input);

/*********************************************************************
** The matrix is broken up into column blocks that are mapped one to a 
** rank.  Each column block is made up of Num_procs smaller square 
** blocks of order block_order.
*********************************************************************/

  Block_order    = order/Num_procs;
  colstart       = Block_order * my_ID;
  Colblock_size  = order * Block_order;
  Block_size     = Block_order * Block_order;

  /* a non-positive tile size means no tiling of the local transpose */
  tiling = (Tile_order > 0) && (Tile_order < order);
  /* test whether tiling will leave threads idle. If so, turn it off */
  concurrency = ceil((double)Block_order/(double)Tile_order);
#ifdef COLLAPSE  
  concurrency *= concurrency;
#endif
  if (tiling && (concurrency < nthread_input)) tiling = 0;

  if (my_ID == root) {
    printf("Parallel Research Kernels version %s\n", PRKVERSION);
    printf("MPI+OpenMP matrix transpose: B = A^T\n");
    printf("Number of ranks      = %d\n", Num_procs);
    printf("Number of threads    = %d\n", omp_get_max_threads());
    printf("Matrix order         = %zu\n", order);
    printf("Number of iterations = %zu\n", iterations);
    if (tiling) {
      printf("Tile size            = %zu\n", Tile_order);
#ifdef COLLAPSE
       printf("Using loop collapse\n");
    }
#endif
    else  printf("Untiled\n");
#ifndef SYNCHRONOUS
    printf("Non-");
#endif
    printf("Blocking messages\n");
  }

  bytes = 2UL * sizeof(double) * order * order;

/*********************************************************************
** Create the column block of the test matrix, the row block of the 
** transposed matrix, and workspace (workspace only if #procs>1)
*********************************************************************/
  A_p = (double *)malloc(Colblock_size*sizeof(double));
  if (A_p == NULL){
    printf(" Error allocating space for original matrix on node %d\n",my_ID);
    error = 1;
  }
  bail_out(error);

  B_p = (double *)malloc(Colblock_size*sizeof(double));
  if (B_p == NULL){
    printf(" Error allocating space for transpose matrix on node %d\n",my_ID);
    error = 1;
  }
  bail_out(error);

  if (Num_procs>1) {
    Work_in_p   = (double *)malloc(2*Block_size*sizeof(double));
    if (Work_in_p == NULL){
      printf(" Error allocating space for work on node %d\n",my_ID);
      error = 1;
    }
    bail_out(error);
    Work_out_p = Work_in_p + Block_size;
  }
  
  /* Fill the original column matrix                                                */
  istart = 0;  

  if (tiling) {
#ifdef COLLAPSE
    #pragma omp parallel for private (i,it,jt) collapse(2)
#else
    #pragma omp parallel for private (i,it,jt)
#endif
    for (j=0; j<Block_order; j+=Tile_order) 
      for (i=0; i<order; i+=Tile_order) 
        for (jt=j; jt<MIN(Block_order,j+Tile_order);jt++) 
          for (it=i; it<MIN(order,i+Tile_order); it++) {
            A(it,jt) = (double) (order*(jt+colstart) + it);
            B(it,jt) = -1.0;
          }
  }
  else {
    #pragma omp parallel for private (i)
    for (j=0;j<Block_order;j++) 
      for (i=0;i<order; i++) {
        A(i,j) = (double) (order*(j+colstart) + i);
        B(i,j) = -1.0;
    }
  }

  for (iter = 0; iter<=iterations; iter++){

    /* start timer after a warmup iteration                                        */
    if (iter == 1) { 
      MPI_Barrier(MPI_COMM_WORLD);
      local_trans_time = wtime();
    }

    /* do the local transpose                                                       */
    istart = colstart; 
    if (!tiling) {
    #pragma omp parallel for private (j)
      for (i=0; i<Block_order; i++) 
        for (j=0; j<Block_order; j++) {
          B(j,i) = A(i,j);
	}
    }
    else {
#ifdef COLLAPSE
      #pragma omp parallel for private (j,it,jt) collapse(2)
#else
      #pragma omp parallel for private (j,it,jt)
#endif
      for (i=0; i<Block_order; i+=Tile_order) 
        for (j=0; j<Block_order; j+=Tile_order) 
          for (it=i; it<MIN(Block_order,i+Tile_order); it++)
            for (jt=j; jt<MIN(Block_order,j+Tile_order);jt++) {
              B(jt,it) = A(it,jt); 
	    }
    }

    for (phase=1; phase<Num_procs; phase++){
      recv_from = (my_ID + phase            )%Num_procs;
      send_to   = (my_ID - phase + Num_procs)%Num_procs;

#ifndef SYNCHRONOUS
      MPI_Irecv(Work_in_p, Block_size, MPI_DOUBLE, 
                recv_from, phase, MPI_COMM_WORLD, &recv_req);  
#endif

      istart = send_to*Block_order; 
      if (!tiling) {
        #pragma omp parallel for private (j)
        for (i=0; i<Block_order; i++) 
          for (j=0; j<Block_order; j++){
	    Work_out(j,i) = A(i,j);
	  }
      }
      else {
#ifdef COLLAPSE
        #pragma omp parallel for private (j,it,jt) collapse(2)
#else
        #pragma omp parallel for private (j,it,jt)
#endif
        for (i=0; i<Block_order; i+=Tile_order) 
          for (j=0; j<Block_order; j+=Tile_order) 
            for (it=i; it<MIN(Block_order,i+Tile_order); it++)
              for (jt=j; jt<MIN(Block_order,j+Tile_order);jt++) {
                Work_out(jt,it) = A(it,jt); 
	      }
      }

#ifndef SYNCHRONOUS  
      MPI_Isend(Work_out_p, Block_size, MPI_DOUBLE, send_to,
                phase, MPI_COMM_WORLD, &send_req);
      MPI_Wait(&recv_req, &status);
      MPI_Wait(&send_req, &status);
#else
      MPI_Sendrecv(Work_out_p, Block_size, MPI_DOUBLE, send_to, phase,
                   Work_in_p, Block_size, MPI_DOUBLE, 
	           recv_from, phase, MPI_COMM_WORLD, &status);
#endif

      istart = recv_from*Block_order; 
      /* scatter received block to transposed matrix; no need to tile */
      #pragma omp parallel for private (i)
      for (j=0; j<Block_order; j++)
        for (i=0; i<Block_order; i++) {
          B(i,j) = Work_in(i,j);
        }

    }  /* end of phase loop  */
  } /* end of iterations */

  local_trans_time = wtime() - local_trans_time;
  MPI_Reduce(&local_trans_time, &trans_time, 1, MPI_DOUBLE, MPI_MAX, root,
             MPI_COMM_WORLD);

  abserr = 0.0;
  istart = 0;
  #pragma omp parallel for private (i)
  for (j=0;j<Block_order;j++) for (i=0;i<order; i++) {
      abserr += ABS(B(i,j) - (double)(order*i + j+colstart));
  }

  MPI_Reduce(&abserr, &abserr_tot, 1, MPI_DOUBLE, MPI_SUM, root, MPI_COMM_WORLD);

  if (my_ID == root) {
    if (abserr_tot < epsilon) {
      printf("Solution validates\n");
      avgtime = trans_time/(double)iterations;
      printf("Rate (MB/s): %lf Avg time (s): %lf\n",1.0E-06*bytes/avgtime, avgtime);
#ifdef VERBOSE
      printf("Summed errors: %f \n", abserr);
#endif
    }
    else {
      printf("ERROR: Aggregate squared error %lf exceeds threshold %e\n", abserr_tot, epsilon);
      error = 1;
    }
  }

  bail_out(error);

  MPI_Finalize();
  exit(EXIT_SUCCESS);

}  /* end of main */

