#include <jemalloc/jemalloc.h>
#include <atomic>
#include <stdint.h>
#include <memory.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <assert.h>
#include <unistd.h>

#ifdef _MSC_VER
#pragma intrinsic(__rdtsc)
uint64_t get_cc_time () {
    return __rdtsc();
}
#else
/* define this somewhere */
#ifdef __i386
__inline__ uint64_t rdtsc() {
     uint64_t x;
       __asm__ volatile ("rdtsc" : "=A" (x));
         return x;
}
#elif __amd64
__inline__ uint64_t rdtsc() {
     uint64_t a, d;
       __asm__ volatile ("rdtsc" : "=a" (a), "=d" (d));
         return a; //(d<<32) | a;
}
#endif


uint64_t get_cc_time () {
   return rdtsc();
}
#endif


// Some primes between 2^63 and 2^64 for various uses.
// source: CityHash
static const uint64_t k0 = 0xc3a5c85c97cb3127ULL;
static const uint64_t k1 = 0xb492b66fbe98f273ULL;
static const uint64_t k2 = 0x9ae16a3b2f90404fULL;

inline uint64_t ShiftMix(uint64_t val) { return val ^ (val >> 47); }

inline int64_t fast_rand()
{
  int64_t now = rdtsc(); //get_cc_time();
  char*   s = (char*)&now; // note first 4 bits are 'LSB' on intel... 
                           // on bigendian machine we want to add 4
                           // LSB is most rand, the higher-order bits
                           // will not change much if at all between
                           // calls...

  const uint8_t a = s[0];
  const uint8_t b = s[4 >> 1];
  const uint8_t c = s[4 - 1];
  const uint32_t y = static_cast<uint32_t>(a) + (static_cast<uint32_t>(b) << 8);
  const uint32_t z = 4 + (static_cast<uint32_t>(c) << 2);
  return ShiftMix(y * k2 ^ z * k0) * k2;
}


template<uint32_t Size>
struct page
{
  public:
    struct slot 
    { 
        int32_t page_id; 
        int16_t page_size; 
        int16_t page_slot;
        char _data[Size];
    };
    page(int16_t page_id)
    {
       _free_count  = 256;
       _alloc_count = 0;
       for( int i = 0; i < 256; ++i )
       {
          slot& s = _slot[i];
          s.page_id   = page_id;
          s.page_size = Size;
          s.page_slot = i;
       }
    }
  
    // last scan pos
    uint8_t _last_pos = 0;

    char*  alloc( uint64_t pos )
    {
       int used = 0;
       while( _reserved[++_last_pos] && used < 256 )
       {
         ++used;
       }
       if( used >= 255 )
       {
          _free_count = 256;
          _alloc_count = 256;
          return nullptr;
       }
       ++_alloc_count;
       _reserved[_last_pos] = 1;
       return _slot[_last_pos]._data;
    }

    void   free( char* s )
    {
    }

    int32_t free_estimate() { return _free_count - _alloc_count; }

    /** updates the estimate */
    int32_t calc_free()     
    {
       auto end = _reserved+256;
       int32_t count = 0;
       for( auto i = _reserved; i < end; ++i )
          count += 1 == *i;

       _alloc_count = count;
       _free_count = 256;
       return count;
    }

    uint32_t _free_count;
    uint32_t _alloc_count;
    uint8_t  _reserved[256];
    slot     _slot[256];
};

template<uint32_t Size>
struct pool
{
   typedef page<Size>*            page_ptr;
   static page_ptr                pages[1024*32];
   static uint8_t                 reserved_pages[1024*32];

   static __thread int32_t        _current;
   static std::atomic<int32_t>    _last_page;

   static int64_t used_estimate()
   {
      int64_t total = 0;
      auto l = _last_page.load();
      for( int i = 0; i < l; ++i )
      {
          total += 256 - pages[i]->calc_free();
      }
      return 256*l - total;
   }
   
   static page<Size>& get_current()
   {
      uint32_t c = _current;
      if( c == -1 ) {
          std::cerr<<"alloc... \n";
         _current = 0;
         return *alloc_page();
      }
      return *pages[c];
   }

   static void free( char* s )
   {
        typename page<Size>::slot* sl = (typename page<Size>::slot*)(s-8);
        page_ptr p = pages[sl->page_id];

        //assert( p );
        //assert( p->_reserved[sl->page_slot] != 0 );

        ++p->_free_count; // not atomic, but we only
                       // need the count for an estimate.
        // not atomic, but then again only one thread should
        // ever be freeing this memory at a time and it cannot
        // be allocated again until it is read as 0...
        p->_reserved[sl->page_slot] = 0;
   }

   /**
    *   Allocates a new page, reserves it for this
    *   thread, and inserts it into the global queue.
    */
   static page_ptr alloc_page()
   {
      int claim = _last_page.fetch_add(1);
      page<Size>* p = (page<Size>*)malloc( sizeof(page<Size>) );
      new (p) page<Size>(claim); // in place construct

      // TODO: convert to CAS loop... this is broken
      pages[claim] = p; // scanners attempting to reclaim pages check for 'null' anyway.. no need
                        // to CAS this.  It is good enough that we reserved the spot atomicaly
                        //
      reserved_pages[_current] = 0; // unreserve the page
      _current = claim;
      reserved_pages[_current] = 1; // unreserve the page
      return p;
   }

   /**
    *  Randomly sample pages looking for one with
    *  free space.  If one is found in less than
    *  4 attempts return it, else return NULL
    *
    *  @param pos - random data used for probing
    */
   static page_ptr claim_page(uint64_t pos)
   {
      auto last_p = _last_page.load( std::memory_order_relaxed );
      for( int i = 0; i < last_p; ++i )
      {
        if( !reserved_pages[i] && pages[i] && pages[i]->free_estimate() > 128 )
        {
           if( 0 == ((std::atomic<uint8_t>*)&reserved_pages[i])->fetch_add(1) )
           {
              reserved_pages[_current] = 0; // unreserve the current page
              _current = i;
              reserved_pages[_current] = 1; // reserve the new page
              return pages[i];
           }
        }
      }
      return alloc_page();
   }

   static char*  alloc()
   {
      char* c = get_current().alloc(0);
      if( c ) 
         return c;
      auto p = claim_page(0);
      return p->alloc(0);
   }
   static bool init;
};
template<uint32_t Size>
bool   pool<Size>::init = []()->bool{
   memset( reserved_pages, 0, sizeof(reserved_pages) );
  return true; 
}();

template<uint32_t Size>
typename pool<Size>::page_ptr    pool<Size>::pages[1024*32];
template<uint32_t Size>
uint8_t                pool<Size>::reserved_pages[1024*32];

template<uint32_t Size>
__thread int32_t        pool<Size>::_current = -1;
template<uint32_t Size>
std::atomic<int32_t>    pool<Size>::_last_page;


   

/**
 *  Goals of algorithm:
 *    - Provide fast, lock-free memory allocation system designed
 *      for systems with a lot of churn.
 *
 *
 *  Algorithm:
 *    b = find_bucket_for_size()
 *    rd = fast_rand() // 2 CPU cycles...city_hash(cpu_counter)
 *    l  = b.current; // lookup thread local cache..
 *    try l.allocate(fr);  // should usually succeed without any locks or atomics
 *
 *    while( b.current = reprobe(rd) ) // up to 4 test + 4 atomic incs + 4 subs + 4 compares
 *        try b.current.allocate(rd)
 *        rd = fast_rand()
 *
 *    all else failed... allocate new
 *        b.current = malloc
 *        init page... 
 *        CAS append to end
 *        ATOMIC ADD to len
 *        b.current.allocate(rd)
 *
 */

pool<64> _pool;




#define BENCH_SIZE ( (1024*128) )
#define ROUNDS 100 



#include <thread>
void malloc_bench( int tid )
{
  std::vector<char*> a(BENCH_SIZE);
  memset( a.data(), 0, a.size() * sizeof(char*));
  for( int x = 0; x < ROUNDS; ++x )
  {
    for( int i = 0; i < BENCH_SIZE; ++i )
    {
      int pos = rand() & 1;
      if( a[i] && pos )
      {
          free(a[i]); 
          a[i]=0;
      }
      else if( !a[i] && pos )
      {
          a[i] = (char*)malloc(64);
      }
    }
  }
}
void bench(int tid)
{
  std::vector<char*> a(BENCH_SIZE);
  memset( a.data(), 0, a.size() * sizeof(char*));
  for( int x = 0; x < ROUNDS; ++x )
  {
    for( int i = 0; i < BENCH_SIZE; ++i )
    {
      int pos = rand() & 1;
      if( a[i] && pos )
      {
          pool<64>::free(a[i]); 
          a[i] = 0;//free(a[i]); 
      }
      else if( !a[i] && pos )
      {
         a[i] = pool<64>::alloc();
      }
    }
  }
}

std::vector<char*>  buffers[16];


void pc_bench_worker( int pro, int con, char* (*do_alloc)(), void (*do_free)(char*)  )
{
  for( int r = 0; r < ROUNDS; ++r )
  {
     // produce some
     for( int i = 0; i < buffers[pro].size(); ++i )
     {
        // don't wrap...
      //  while( buffers[pro][i] ) usleep(0);
        buffers[pro][i] = do_alloc();
     }
     usleep( 100 );
     for( int i = 0; i < buffers[pro].size(); ++i )
     {
     //   while( !buffers[con][i] ) usleep(0);
        if( buffers[con][i] )
        {
           do_free(buffers[con][i]);
           buffers[con][i] = 0;
        }
     }
     
  }
}

void pc_bench(char* (*do_alloc)(), void (*do_free)(char*)  )
{
  for( int i = 0; i < 16; ++i )
  {
    buffers[i].resize( BENCH_SIZE );
    memset( buffers[i].data(), 0, 8 * BENCH_SIZE );
  }
  int i = 0;
  std::thread a( [=](){ pc_bench_worker( 1, 1, do_alloc, do_free ); } );
  std::thread b( [=](){ pc_bench_worker( 2, 2, do_alloc, do_free ); } );
  std::thread c( [=](){ pc_bench_worker( 3, 3, do_alloc, do_free ); } );
  std::thread d( [=](){ pc_bench_worker( 4, 4, do_alloc, do_free ); } );
  std::thread e( [=](){ pc_bench_worker( 5, 5, do_alloc, do_free ); } );
  std::thread f( [=](){ pc_bench_worker( 6, 6, do_alloc, do_free ); } );
  std::thread g( [=](){ pc_bench_worker( 7, 7, do_alloc, do_free ); } );
  std::thread h( [=](){ pc_bench_worker( 8, 8, do_alloc, do_free ); } );

  a.join();
  b.join();
  c.join();
  d.join();
  e.join();
  f.join();
  g.join();
  h.join();
}
void pc_bench_st(char* (*do_alloc)(), void (*do_free)(char*)  )
{
  for( int i = 0; i < 16; ++i )
  {
    buffers[i].resize( BENCH_SIZE );
    memset( buffers[i].data(), 0, 8 * BENCH_SIZE );
  }
  int i = 0;
  std::thread a( [=](){ pc_bench_worker( 1, 1, do_alloc, do_free ); } );
  /*
  std::thread b( [=](){ pc_bench_worker( 2, 2, do_alloc, do_free ); } );
  std::thread c( [=](){ pc_bench_worker( 3, 3, do_alloc, do_free ); } );
  std::thread d( [=](){ pc_bench_worker( 4, 4, do_alloc, do_free ); } );
  std::thread e( [=](){ pc_bench_worker( 5, 5, do_alloc, do_free ); } );
  std::thread f( [=](){ pc_bench_worker( 6, 6, do_alloc, do_free ); } );
  std::thread g( [=](){ pc_bench_worker( 7, 7, do_alloc, do_free ); } );
  std::thread h( [=](){ pc_bench_worker( 8, 8, do_alloc, do_free ); } );
  */

  a.join();
  /*
  b.join();
  c.join();
  d.join();
  e.join();
  f.join();
  g.join();
  h.join();
  */
}

char* do_malloc(){ return (char*)malloc(64); }
void  do_malloc_free(char* c){ free(c); }
char* do_hash_malloc(){ return pool<64>::alloc(); }
void  do_hash_free(char* c){ pool<64>::free(c); }


int main( int argc, char** argv )
{
  if( argc > 1 )
  {
    std::cerr<<"hash malloc\n";
    pc_bench_st( do_hash_malloc, do_hash_free );
   // pc_bench( do_hash_malloc, do_hash_free );
  }
  else
  {
    std::cerr<<"jemalloc\n";
    pc_bench_st( do_malloc, do_malloc_free );
  }
  return 0;
}


  /*
  int64_t count[256];
  memset( count, 0, sizeof(count) );
  for( uint64_t i = 0; i < 1000ll*1000ll*1000ll; ++i )
  {
    int64_t h;
    h = fast_rand();

    uint8_t* hc = (uint8_t*)&h;
    for( int x = 0; x < 8; ++x )
       count[hc[x]]++;
  }
  for( int i = 0; i < 256; ++i )
  {
      std::cerr<<"i:"<<i<<"  "<<count[i]<<"\n";
  }
  */
#if 0
  if( arg > 1 )
  {
  std::cerr<<"hash bench\n";

  std::thread ta([=](){bench(1);});
  std::thread tb([=](){bench(2);});
  std::thread tc([=](){bench(3);});
  std::thread td([=](){bench(4);});
  std::thread te([=](){bench(5);});
  /*
  std::thread tc(bench);
  std::thread td(bench);
  */
  bench(5);
  td.join();
  tc.join();
  tb.join();
  ta.join();
  te.join();
  }
  else
  {
  std::cerr<<"malloc bench\n";
  std::thread ta([=](){malloc_bench(1);});
  std::thread tb([=](){malloc_bench(2);});
  std::thread tc([=](){malloc_bench(3);});
  std::thread td([=](){malloc_bench(4);});
  std::thread te([=](){malloc_bench(5);});
  malloc(5);
  /*
  std::thread tc(bench);
  std::thread td(bench);
  */
  td.join();
  tc.join();
  tb.join();
  ta.join();
  te.join();
  }

  return -1;
  
  /*
  for( int64_t i = 0; i < 1000ll*1000ll*1000ll; ++i )
  {
    auto r = rand() % 1000;
    if( a[r] ) {
        assert( pool<64>::free(a[r]) );
        a[r] = nullptr;
    } else {
        a[r] =  pool<64>::alloc();
    }
  }
  */
  return 0;
  #endif







