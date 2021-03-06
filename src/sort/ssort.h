#ifndef __SA_SAMPLESORT
#define __SA_SAMPLESORT

#include <mpi.h>

namespace ssort {

template <typename _Iter, typename _Compare>
void samplesort(_Iter begin, _Iter end, _Compare comp, MPI_Datatype mpi_dtype,
                int numprocs, int myid, MPI_Comm comm = MPI_COMM_WORLD);
}

#include "ssort.hpp"

#endif
