fc_malloc
=========

Super Fast, Lock-Free, Wait-Free, CAS-free, thread-safe, memory allocator. 

Design
================== 

The key to developing fast multi-threaded allocators is eliminating 
lock-contention and false sharing.  Even simple atomic operations and
spin-locks can destroy the performance of an allocation system.  The real
challenge is that the heap is a multi-producer, multi-consumer resource 
where all threads need to read and write the common memory pool.

With fc_malloc I borrowed design principles from the LMAX disruptor and
assigned a dedicated thread for moving free blocks from all of the other
threads to the shared pool.  This makes all threads 'single producers' of
free blocks and therefore it is possible to have a lock-free, wait-free 
per-thread free list.   This also makes a single producer of 'free blocks'
which means that blocks can be aquired with a single-producer, multiple
consumer pattern.  

When there is a need for more memory and existing free-lists are not sufficent,
each thread maps its own range from the OS in 4 MB chunks. Allocating from
this 'cache miss' is not much slower than allocating stack space and
requires no contention.  Requests for larger than 4MB are allocated direclty
from the OS via mmap.  

Initial Benchmarks
==================

Testing memory allocation systems can be very difficulty and 'artificial tests',
are not always the most accurate predictors of real world performance, but I 
sought to develop a test that would stress the allocation system, particularlly
in multi-threaded environments.

The test I came up with creates 1 array per thread containing space for 500K 
allocations.  I then assigned each thread the job of randomly allocating 
empty slots in 1 array and randomly deallocating random slots in another array. 
The result is a 'random' set of producer-consumer threads.

Each allocation was 128 bytes.  Future versions of this benchmark will include
random sizes as well.  


| Benchmark                  | glibc       |  jemalloc   |   fc_malloc |
|----------------------------|-------------|-------------|-------------|
| Random Single Threaded     | 5.8s        |  4.5s       |  2.6s       |
| Random Multi Threaded (10) | 18.2s       |  13.6s      |  6.8s       |

Threads|fcalloc (s)|jemalloc(s)|fcram(MB)|jeram(mb)
---|---|---|---|---
1|4.8|9.7|97|84.3
2|5.9|14.8|120|104
3|6.5|16.8|145|123
4|7|18|167|142
5|8|18.9|185.5|160
6|8.7|20.3|214.3|189
7|9.9|22.9|238|212
8|11.4|25.2|257|224
9|12.5|26.1|278|244
10|12.9|27.9|308|270




As you can see from the results fc_malloc is over 2x faster than the
stock malloc even for single threaded cases.  For multi-threaded cases
it is 2.6x faster than the stock allocator.   The real test though is
the comparison to jemalloc which is generally considered one of the
highest performing alternative allocators available.  Here fc_malloc
is still 2x faster in the multi-threaded test.

