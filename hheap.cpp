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


template<uint32_t Size, uint32_t NumSlots>
struct page
{
  public:
    struct slot 
    { 
        int32_t page_id;     // used by free to find the page in the pool
        int16_t pool_id;     // used by free to find the pool
        uint8_t page_slot;   // the slot in the page in the pool
        uint8_t alignment;   // 8 if reserved, 0 if free... byte _data[alignment-1] = alignment.
        char    _data[Size]; // alignment helps us find the page_id/pool_id when allocated aligned objects.
    };

    page(int16_t page_id, int16_t pool_id)
    {
       _free_count  = NumSlots;
       _alloc_count = 0;
       for( int i = 0; i < NumSlots; ++i )
       {
          slot& s = _slot[i];
          s.page_id = page_id;
          s.pool_id = pool_id;
          s.page_slot = i;
       }
    }

    char*  alloc( uint64_t pos )
    {
       int used = 0;
       // TODO: compare 8 bytes at a time, increment pointers
       // instead of 'indexes' will make this faster.
       // TODO: ++_last_pos only works if _last_pos is uint8 and size is 256... 
       //       must be fixed!!!!
       while( _reserved[++_last_pos] && used < NumSlots )
       {
         ++used;
       }
       if( used >= NumSlots )
       {
          _free_count = NumSlots;
          _alloc_count = NumSlots;
          return nullptr;
       }
       ++_alloc_count;
       _reserved[_last_pos] = 1;
       return _slot[_last_pos]._data;
    }

    int32_t free_estimate() { return _free_count - _alloc_count; }

    /** updates the estimate */
    int32_t calc_free()     
    {
       auto end = _reserved+NumSlots;
       int32_t count = 0;
       for( auto i = _reserved; i < end; ++i )
          count += 1 == *i;

       _alloc_count = count;
       _free_count = NumSlots;
       return count;
    }

    uint32_t _free_count;
    uint32_t _alloc_count;
    uint8_t  _reserved[NumSlots];
    // last scan pos
    uint8_t  _last_pos = 0;
    slot     _slot[NumSlots];
};


/**
 *   Manages a mini sorted array of size 256 of
 *   available slots.  
 *   that can serve sizes between Size and Size/MaxSlots.
 *
 *   The heap sorts all slots by size (smallest first),
 *   the max size slot is tracked as well as the
 *   max free slot.
 *
 *   When a slot is released, its size is compared
 *   against the current largest free block and then
 *   swapped. This can be done with a CAS. The
 *   total 'free' bytes is also tracked.
 *
 *   When a thread needs to allocate a new chunk that
 *   is too big for the current page (due to fragmentation),
 *   it will scan the list of dynamic_pages for the
 *   the block with the 'most total free' and has
 *   a fragment of acceptable size.  It will then
 *   take over this 'heap', clean it up and return
 *   the allocated buffer.
 */
template<uint32_t Size, uint32_t MaxSlots=256> 
struct dynamic_page
{
   // TODO: assert MaxSlots is power of 2
    struct slot // sizeof(slot) == 16... keep things aligned.
    {
       int32_t  page_id;    // identifies page in the dynamic_pool
       uint16_t pool_id;    // the dynamic pool this slot is from
       uint8_t  merged;     // has this slot been merged 
       uint8_t  reserved;   // 8 for reserved, 0 for free, aka alignment,
                            // if the slot has alignment 
    };

    struct slot_index
    {
       uint32_t size;     // bytes allocated for this chunk.
       slot_ptr 
    };
    typedef slot* slot_ptr;

    char      _buffer[Size];

    // array of slot pointers sorted by slot size.
    slot_ptr  _slist[MaxSlots];   // array of allocated blocks..
    size_t    _slist_size;        // valid slots in _slist
    size_t    _max_free_slot;     // updated by free() never read by allocator, written to by allocator 
                                  // when ever it divides the largest block.. which shouldn't happen
                                  // often because it chooses best fit first.
    size_t    _total_free;        // total free bytes... estimate of free bytes.
    size_t    _total_free_slots;  // estimate of free slots  _total_free / _total_free_slots 
                                  // gives an estimate of fragmentation.

    dynamic_page( uint16_t page_id )
    {
      memset( _slist,0,sizeof(slist) );
      memset( _buffer,0,sizeof(buffer) );
      _slist->page_id  = page_id;
      _slist->size     = Size - sizeof(slot);
      _slist->reserved = 0;

      _slist[0] = (slot*)_reserved_slots;
      _slist_size = 1;
    }

    char*     alloc( uint32_t size )
    {
       slot_ptr canidate = nullptr;
       for( size_t s = 0; s < _slist_size; ++s )
       {
          slot_ptr cur = _slist[s];
          if( cur->reserved || cur->merged ) continue;
          if( cur->size > size )
          {
            canidate = cur;
            continue;
          }
          slot_ptr next = cur + 1 + cur->size / sizeof(slot);
          if( !next->reserved )
          {
              assert( !next->merged );
              // merge next into current block
              cur->size    += next->size + sizeof(slot);
              // mark 'next' for removal from the slist heap.
              next->merged = true;
          }

          // if we have enough to create a new 64 byte block...
          // split the block
          if( canidate && (canidate->size-64) > size && _slist_size < MaxSlots )
          {
             _slist[_slist_size]
          }
          break;
       }
       return (char*)(canidate+1);
    }
    void    free( slot_ptr s )
    {
        assert( s->reserved == 1 );
        s->reserved = 0;
        total_free += s->size + sizeof(slot);
        if( s->size > _max_free_slot )
        {
           // note... max free size is just an estimate because
           // we could do a CAS here... which would make sure
           // allocation remains effecient... CAS would only
           // be required when _max_free_slot was growing...
           // which would grealty reduce the contention.
           _max_free_slot = s->size;
        }
    }
};










template<uint16_t PoolId,uint32_t Size,uint32_t SlotsPerPage,uint32_t MaxPages=1024*32>
struct pool
{
   typedef page<Size>*            page_ptr;
   static page_ptr                pages[MaxPages]
   static uint8_t                 reserved_pages[MaxPages]

   static __thread int32_t        _current;
   static std::atomic<int32_t>    _last_page;

   /**
    * Estimates the total number of slots that have not 
    * been freed.
    */
   static int64_t used_estimate()
   {
      int64_t total = 0;
      auto l = _last_page.load();
      for( int i = 0; i < l; ++i )
      {
          total += SlotsPerPage - pages[i]->calc_free();
      }
      return SlotsPerPage*l - total;
   }
   
   static page<Size>& get_current()
   {
      uint32_t c = _current;
      if( c == -1 ) {
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
        // 
        // The value 'should be 1', if it is already 0 then we have
        // 'double free' error.
        //assert( p->_reserved[sl->page_slot] == 1 );
        p->_reserved[sl->page_slot] = 0;
   }

   /**
    *   Allocates a new page, reserves it for this
    *   thread, and inserts it into the global queue.
    */
   static page_ptr alloc_page()
   {
      int claim = _last_page.fetch_add(1);

      // TODO: this should allocate via mmap() instead of malloc because
      // our goal is to replace malloc.
      page<Size>* p = (page<Size>*)malloc( sizeof(page<Size>) );
      new (p) page<Size>(claim, PoolId); // in place construct

      // TODO: convert to CAS loop... this is broken
      pages[claim] = p; // scanners attempting to reclaim pages check for 'null' anyway.. no need
                        // to CAS this.  It is good enough that we reserved the spot atomicaly
                        
   //   reserved_pages[_current] = 0; // unreserve the page
      _current = claim;
      reserved_pages[claim] = 1; // unreserve the page
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
      assert( _current >= 0 );
      _reserved[_current] = 0;

      auto last_p = _last_page.load( std::memory_order_relaxed );
      for( int i = 0; i < last_p; ++i )
      {
        if( !reserved_pages[i] && pages[i] && pages[i]->free_estimate() > 128 )
        {
           if( 0 == ((std::atomic<uint8_t>*)&reserved_pages[i])->fetch_add(1) )
           {
         //     reserved_pages[_current] = 0; // unreserve the current page
              _current = i;
         //   unnecessary we already clamed it.
         //     reserved_pages[_current] = 1; // reserve the new page
              return pages[i];
           }
        }
      }
      return alloc_page();
   }

   /**
    *   Attempts to allocate in the current page, else
    *   finds a free page and attempts to allocate there.
    */
   static char*  alloc()
   {
      char* c = get_current().alloc(0);
      if( c ) return c;
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







