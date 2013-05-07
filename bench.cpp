#include "fixed_pool.hpp"
#include <thread>
#include <string.h>
#include <stdio.h>
#include <iostream>
#include <sstream>
#define BENCH_SIZE ( (1024*16*2) )
#define ROUNDS 3000

/*  SEQUENTIAL BENCH
int main( int argc, char** argv )
{
  if( argc == 2 && argv[1][0] == 'S' )
  {
     printf( "fp_malloc\n");
     for( int i = 0; i < 50000000; ++i )
     {
        char* test = fp_malloc( 128 );
        assert( test != nullptr );
        test[0] = 1;
        free2( test );
     }
  }
  if( argc == 2 && argv[1][0] == 's' )
  {
     printf( "malloc\n");
     for( int i = 0; i < 50000000; ++i )
     {
        char* test = (char*)malloc( 128 );
        assert( test != nullptr );
        test[0] = 1;
        free( test );
     }
  }
  fprintf( stderr, "done\n");
 // sleep(5);
  return 0;
}
*/
/* RANDOM BENCH */
std::vector<int64_t*>  buffers[16];
void pc_bench_worker( int pro, int con, char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  int64_t total_alloc = 0;
  int64_t total_free = 0;
  int64_t total_block_alloc = 0;
  int64_t total_free_alloc = 0;

  for( int r = 0; r < ROUNDS; ++r )
  {
      for( size_t x = 0; x < BENCH_SIZE/4 ; ++x )
      {
         uint32_t p = rand() % buffers[pro].size();
         if( !buffers[pro][p] )
         {
           uint64_t si = 10000;//16 +rand()%(1024); //4000;//32 + rand() % (1<<16);
           total_alloc += si;
           int64_t* r = (int64_t*)do_alloc( si );
      //     block_header* bh = ((block_header*)r)-1;
     //      assert( bh->size() >= si + 8 );
      //     fprintf( stderr, "alloc: %p  %llu  of %llu  %u\n", r, si, bh->size(), bh->_size );
           assert( r != nullptr );
         //  assert( r[0] != 99 ); 
         
           memset( r, 0x00, si );
        //   r[0] = 99;
    //       total_block_alloc += r[1] = ((block_header*)r)[-1].size();
           buffers[pro][p] = r;
         }
      }
      for( size_t x = 0; x < BENCH_SIZE/4 ; ++x )
      {
         uint32_t p = rand() % buffers[con].size();
         assert( p < buffers[con].size() );
         assert( con < 16 );
         assert( con >= 0 );
         if( buffers[con][p] ) 
         { 
          // assert( buffers[con][p][0] == 99 ); 
          // buffers[con][p][0] = 0; 
         //  total_free += buffers[con][p][0];
         //  total_free_alloc += buffers[con][p][1];
           do_free((char*)buffers[con][p]);
           buffers[con][p] = nullptr;
         }
      }
      /*
      fprintf( stderr, "\n Total Alloc: %lld   Total Free: %lld   Net: %lld\n", total_alloc, total_free, (total_alloc-total_free) );
      fprintf( stderr, "\n Total Block Size: %lld   Total Free Blocks: %lld   Net: %lld\n\n", total_block_alloc, total_free_alloc, (total_block_alloc-total_free_alloc) );
      auto needed = (total_alloc-total_free);
      auto used = (total_block_alloc-total_free_alloc);
      auto wasted = used - needed;
      fprintf( stderr, "\n Total Waste: %lld    %f\n\n", wasted,  double(used)/double(needed) );
      */
  }
}


void pc_bench(int n, char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  for( int i = 0; i < 16; ++i )
  {
    buffers[i].resize( BENCH_SIZE );
    memset( buffers[i].data(), 0, 8 * BENCH_SIZE );
  }

  std::thread* a = nullptr;
  std::thread* b = nullptr;
  std::thread* c = nullptr;
  std::thread* d = nullptr;
  std::thread* e = nullptr;
  std::thread* f = nullptr;
  std::thread* g = nullptr;
  std::thread* h = nullptr;
  std::thread* i = nullptr;
  std::thread* j = nullptr;


 int s = 1;
  switch( n )
  {
     case 10:
     a = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 9:
      b = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 8:
      c = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 7:
      d = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 6:
     e = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 5:
     f = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 4:
      g = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 3:
      h = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 2:
      i = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 1:
      j = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
  }
  if(a)
  a->join();
  if(b)
  b->join();
  if(c)
  c->join();
  if(d)
  d->join();
  if(e)
  e->join();
  if(f)
  f->join();
  if(g)
  g->join();
  if(h)
  h->join();
  if(i)
  i->join();
  if(j)
  j->join();

}
void pc_bench_st(char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  for( int i = 0; i < 16; ++i )
  {
    buffers[i].resize( BENCH_SIZE );
    memset( buffers[i].data(), 0, 8 * BENCH_SIZE );
  }
  //int i = 0;
  pc_bench_worker( 1, 1, do_alloc, do_free );
}
//#include <tbb/scalable_allocator.h>

char* do_malloc(int s)
{ 
    return (char*)::malloc(s); 
//   return (char*)scalable_malloc(s);
}
void  do_malloc_free(char* c)
{ 
//    scalable_free(c);
   ::free(c); 
}

char* do_fc_malloc(int s)
{ 
  return (char*)fp_malloc(s);
//    return (char*)fc_malloc(s); 
//   return (char*)scalable_malloc(s);
}
void  do_fc_free(char* c)
{ 
  fp_free((void*)c);
//    scalable_free(c);
//   fc_free(c); 
}


int main( int argc, char** argv )
{
  /*
  char* a = static_heap.alloc32();
  char* b = static_heap.alloc32();
  char* c = static_heap.alloc32();
  fprintf( stderr, "%p %p %p\n", a, b, c );
  static_heap.free32(b);
  char* d = static_heap.alloc32();
  fprintf( stderr, "%p %p %p\n", d, b, c );
  return 0;
  */

  if( argc > 2 && argv[1][0] == 'm' )
  {
    std::cerr<<"malloc multi\n";
    pc_bench( atoi(argv[2]), do_malloc, do_malloc_free );
    return 0;
  }
  if( argc > 2 && argv[1][0] == 'M' )
  {
    std::cerr<<"hash malloc multi\n";
//    pc_bench( atoi(argv[2]), do_fp_malloc, do_fp_free );
    pc_bench( atoi(argv[2]), do_fc_malloc, do_fc_free );
    return 0;
  }
  if( argc > 1 && argv[1][0] == 's' )
  {
    std::cerr<<"malloc single\n";
    pc_bench_st( do_malloc, do_malloc_free );
    return 0;
  }
  if( argc > 1 && argv[1][0] == 'S' )
  {
    std::cerr<<"hash malloc single\n";
    pc_bench_st( do_fc_malloc, do_fc_free );
    return 0;
  }
  std::string line;
  std::getline( std::cin, line );
    std::vector<char*> data;
  while( !std::cin.eof() )
  {
    std::stringstream ss(line);
    std::string cmd;

    ss >> cmd;
    if( cmd == "a" ) // allocate new data
    {
      int64_t bytes;
      ss >> bytes;
      data.push_back( (char*)fp_malloc( bytes ) );
    }
    if( cmd == "f" ) // free data at index
    {
      int64_t idx;
      ss >> idx;
      fp_free( data[idx] );
      data.erase( data.begin() + idx );
    }
    if( cmd == "c" ) // print cache
    {
    //  thread_allocator::get().print_cache();
    }
    if( cmd == "p" ) // print heap
    {

    }
    if( cmd == "l" ) // list data
    {
       fprintf( stderr, "ID]  ptr  _size   _prev_size\n");
       fprintf( stderr, "-----------------------------\n");
       for( size_t i = 0; i < data.size(); ++i )
       {
     //     block_header* bh = reinterpret_cast<block_header*>(data[i]-8);
          fprintf( stderr, "%d]  %p \n", int(i), data[i]);

       }
    }
    std::getline( std::cin, line );
  }
  return 0;
}
#if 0
  printf( "alloc\n" );
  char* tmp = fp_malloc( 61 );
  usleep( 1000 );
  char* tmp2 = fp_malloc( 134 );
  usleep( 1000 );
  char* tmp4 = fp_malloc( 899 );
  printf( "a %p  b %p   c %p\n", tmp, tmp2, tmp4 );

  usleep( 1000 );

  printf( "free\n" );
  free2( tmp );
  usleep( 1000 );
  free2( tmp2 );
  usleep( 1000 );
  free2( tmp4 );

  usleep( 1000*1000 );

  printf( "alloc again\n" );
  char* tmp1 = fp_malloc( 61 );
  usleep( 1000 );
  char* tmp3 = fp_malloc( 134 );
  usleep( 1000 );
  char* tmp5 = fp_malloc( 899 );
  printf( "a %p  b %p   c %p\n", tmp1, tmp3, tmp5 );
  free2( tmp1 );
  free2( tmp3 );
  free2( tmp4 );

  usleep( 1000*1000 );

  return 0;
}
#endif






