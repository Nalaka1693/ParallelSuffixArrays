#include <algorithm>
#include <numeric>
#include <mpi.h>

#include "assert.h"

// TODO for implementation:
//
// handle case where some local arrays are of size 0.
//   need to change: local splitting
//                   gathering the local splitters
//                   iterating through sorted allsamples
//
// handle case where many of the global splitters are equal
//   (the code should still work, but it definitely won't distribute the data
//    evenly; some node will certainly not recieve any data)
//
// when sorting bucket_elems, it could be more efficient to do a p-way merge
// instead of using std::sort
//
// remove asserts + assert.h

// NOTES:
// Here I make the assumption that the sizes of the input arrays summed across
// all processors can be held in an int.
// The reason I do this is because MPI calls require counts and displacements
// to be ints, e.g. see MPI_Alltoallv.
// *** This is a big problem if we want to sort big arrays! ***
// There are two possible fixes:
//   (1) Send the arrays with a larger MPI_Datatype 
//   (2) Send the arrays in chunks.
// (See http://stackoverflow.com/questions/23201522/how-can-i-pass-long-and-or-unsigned-integers-to-mpi-arguments?rq=1 )
// In either case, we'll probably want to write wrappers for the MPI functions
// that can handle passing around large arrays. TODO


namespace ssort {

int *exclusive_sum(int *arr, size_t n) {
  int *sums = new int[n];
  sums[0] = 0;
  for (size_t i = 1; i < n; ++i)
    sums[i] = sums[i-1] + arr[i-1];
  return sums;
}

// Return the length of the intersection of [l1, r1) and [l2, r2)
unsigned interval_overlap(unsigned l1, unsigned r1,
                          unsigned l2, unsigned r2) {
  if (l2 < l1) {
    std::swap(l1, l2);
    std::swap(r1, r2);
  }

  if (r1 <= l2) return 0;
  else if (r1 >= r2) return r2 - l2;
  else return r1 - l2;
}

// Return array of p-1 splitter elements, where p is the number of processors.
// 
// Assumes that p^2 is a reasonable number of elements to hold and sort on one
// processor
template <typename _Iter, typename _Compare>
void *get_splitters(_Iter begin, _Iter end,
                    _Compare comp, MPI_Datatype mpi_dtype,
                    int numprocs, int myid) {
  typedef typename std::iterator_traits<_Iter>::value_type value_type;

  const unsigned size = std::distance(begin, end);
  const unsigned sample_size = numprocs - 1;

  // get p-1 local splitters, where p is the number of processors
  value_type *sample = new value_type[sample_size];
  _Iter s_pos = begin;
  const unsigned jump = size / (sample_size+1);
  const unsigned leftover = size % (sample_size+1);
  for (unsigned i = 0; i < sample_size; ++i) {
    s_pos += jump + (i < leftover);
    assert(begin <= s_pos-1 && s_pos-1 < end);
    sample[i] = *(s_pos-1);
  }

  // send local splitters to processor 0
  value_type *all_samples = NULL;
  if (myid == 0)
    all_samples = new value_type[numprocs*sample_size];
  MPI_Gather(sample, sample_size, mpi_dtype,
             all_samples, sample_size, mpi_dtype, 0, MPI_COMM_WORLD);

  // get and broadcast p-1 global splitters, placing them in sample array
  if (myid == 0) {
    std::sort(all_samples, all_samples + numprocs*sample_size, comp);

    unsigned as_pos = 0;
    for (unsigned i = 0; i < sample_size; ++i) {
      as_pos += sample_size;
      assert(0 <= as_pos-1 && as_pos-1 < numprocs*sample_size);
      sample[i] = all_samples[as_pos-1];
    }

    /*
    printf("Splitters:");
    for (unsigned i = 0; i < sample_size; ++i)
      printf(" %d: %d,", i, sample[i]);
    printf("\n");
    */
  }

  MPI_Bcast(sample, sample_size, mpi_dtype, 0, MPI_COMM_WORLD);

  delete[] all_samples;

  return (void *)sample;
}

// Place input data into p buckets and give bucket i to processor i.
// Buckets are not guaranteed to be evenly sized. 
template <typename _Iter, typename _Compare>
void *get_buckets(_Iter begin, _Iter end, _Compare comp,
                  int *bucket_size_ptr,
                  MPI_Datatype mpi_dtype, int numprocs, int myid) {
  typedef typename std::iterator_traits<_Iter>::value_type value_type;
  const int num_splitters = numprocs-1;

  value_type *splitters =
    (value_type *)get_splitters(begin, end, comp, mpi_dtype, numprocs, myid);

  // split local data into p buckets based on global splitters
  int *send_split_counts = new int[numprocs];
  _Iter s_pos = begin;
  int i = 0;
  while (i < num_splitters) {
    _Iter s_pos_next = std::lower_bound(s_pos, end, splitters[i], comp);
    send_split_counts[i] = std::distance(s_pos, s_pos_next);
    s_pos = s_pos_next;
    ++i;
  }
  send_split_counts[num_splitters] = std::distance(s_pos, end);

  // processor i will receive all elements in bucket i across all processors,
  // so send bucket sizes in order to know how much space to allocate
  int *recv_split_counts = new int[numprocs];
  MPI_Alltoall(send_split_counts, 1, MPI_INT,
               recv_split_counts, 1, MPI_INT, MPI_COMM_WORLD);

  int *send_displacements = exclusive_sum(send_split_counts, numprocs); 
  int *recv_displacements = exclusive_sum(recv_split_counts, numprocs); 

  *bucket_size_ptr =
    recv_displacements[numprocs-1] + recv_split_counts[numprocs-1];
  value_type *bucket_elems = new value_type[*bucket_size_ptr];

  // send bucket elements
  MPI_Alltoallv(begin, send_split_counts, send_displacements, mpi_dtype,
                bucket_elems, recv_split_counts, recv_displacements, mpi_dtype,
                MPI_COMM_WORLD);

  delete[] splitters;
  delete[] send_split_counts;
  delete[] recv_split_counts;
  delete[] send_displacements;
  delete[] recv_displacements;

  return (void *)bucket_elems;
}

// Redistribute bucket elements to original input array (begin to end)
template <typename _Iter>
void redistribute(_Iter begin, _Iter end,
                  void *bucket, unsigned bucket_size,
                  MPI_Datatype mpi_dtype, int numprocs, int myid) {
  typedef typename std::iterator_traits<_Iter>::value_type value_type;
  value_type *bucket_elems = (value_type *)bucket;

  int local_sizes[2];
  local_sizes[0] = std::distance(begin, end); // size of original input array
  local_sizes[1] = bucket_size;

  int *all_sizes = new int[2*numprocs];

  MPI_Allgather(local_sizes, 2, MPI_INT, all_sizes, 2, MPI_INT, MPI_COMM_WORLD);

  int *send_counts = new int[numprocs];
  int *recv_counts = new int[numprocs];

  int global_my_orig_begin = 0;
  int global_my_bucket_begin = 0;
  for (int i = 0; i < myid; ++i) {
    global_my_orig_begin += all_sizes[2*i];
    global_my_bucket_begin += all_sizes[2*i+1];
  }
  int global_my_orig_end = global_my_orig_begin + all_sizes[2*myid];
  int global_my_bucket_end = global_my_bucket_begin + all_sizes[2*myid+1];

  int curr_orig_begin = 0;
  int curr_bucket_begin = 0;
  for (int i = 0; i < numprocs; ++i) {
    int curr_orig_end = curr_orig_begin + all_sizes[2*i];
    send_counts[i] = 
      interval_overlap(curr_orig_begin, curr_orig_end, 
                       global_my_bucket_begin, global_my_bucket_end);
    curr_orig_begin = curr_orig_end;

    int curr_bucket_end = curr_bucket_begin + all_sizes[2*i+1];
    recv_counts[i] = 
      interval_overlap(curr_bucket_begin, curr_bucket_end, 
                       global_my_orig_begin, global_my_orig_end);
    curr_bucket_begin = curr_bucket_end;
  }

  int *send_displacements = exclusive_sum(send_counts, numprocs); 
  int *recv_displacements = exclusive_sum(recv_counts, numprocs); 

  MPI_Alltoallv(bucket_elems, send_counts, send_displacements, mpi_dtype,
                begin, recv_counts, recv_displacements, mpi_dtype,
                MPI_COMM_WORLD);

  delete[] all_sizes;
  delete[] send_counts;
  delete[] recv_counts;
  delete[] send_displacements;
  delete[] recv_displacements;
}

// sort elements across all processors, placing the results back into the input
// array.
template <typename _Iter, typename _Compare>
void samplesort(_Iter begin, _Iter end, _Compare comp,
                MPI_Datatype mpi_dtype, int numprocs, int myid) {
  // sort locally
  std::sort(begin, end, comp);

  if (numprocs <= 1)
    return;

  typedef typename std::iterator_traits<_Iter>::value_type value_type;

  int bucket_size;
  value_type *bucket_elems =
    (value_type *)get_buckets(begin, end, comp, &bucket_size, 
                              mpi_dtype, numprocs, myid);

  // sort bucket elements
  std::sort(bucket_elems, bucket_elems + bucket_size, comp);

  //printf("Proc %d: bucket holds %d to %d\n", myid, bucket_elems[0], bucket_elems[bucket_size-1]);

  redistribute(begin, end, bucket_elems, bucket_size, 
               mpi_dtype, numprocs, myid);

  //printf("Proc %d: redistr holds %d to %d\n", myid, *begin, *(end-1));

  delete[] bucket_elems;
}

} // end namespace