/**
 *   Each thread has its own 'arena' where it can allocate 'new' blocks of what ever size it needs (buckets). After
 *   a thread is done with memory it places it in a garbage collection queue.
 *
 *   The garbage collector follows each threads trash bin and moves the blocks into a recycled list that
 *   all other threads can pull from.
 *
 *   The garbage collector can grow these queues as necessary and shrink them as time progresses.
 */

#include <vector>
#include <unordered_set>

//#include "mmap_alloc.hpp"
#include "disruptor.hpp"
#include <thread>
#include "fast_rand.cpp"


using namespace disruptor;

#define PAGE_SIZE (4*1024*1024)
#define BENCH_SIZE ( (2024) )
#define ROUNDS 200000 
#define LOG2(X) ((unsigned) (8*sizeof (unsigned long long) - __builtin_clzll((X)) - 1))
#define NUM_BINS 22 // log2(PAGE_SIZE)

class block_header
{
   public:
      block_header* next()const 
      { 
         if( _size > 0 ) return reinterpret_cast<block_header*>(&_data+_size); 
         else return nullptr;
      }
      block_header* prev()const 
      { 
         if( _prev_size == 0 ) return nullptr;
         reinterpret_cast<block_header*>(reinterpret_cast<char*>(this) - _prev_size - 8);
      }

      char*         data()      { return &_data;     }
      size_t        size()const { return abs(_size); }

      /** create a new block at p and return it */
      block_header* split_after( size_t s )
      {
         if( size()-8-s < 32 ) return nullptr; // no point in splitting to less than 32 bytes

         block_header* n = reinterpret_cast<block_header*>(data()+s);
         n->_prev_size   = s;
         n->_size        = size() -s -8;
         if( _size < 0 ) n->_size = -n->_size; // we just split the tail
         _size = s; // this node now has size s
         return n;
      }

      // merge this block with next, return head of new block.
      block_header* merge_next()
      {
         auto nxt = next();
         if( !nxt ) return this;
         _size += 8 + nxt->size();
         auto nxt_nxt = nxt->next();
         if( nxt_nxt ) 
         {
             nxt_nxt->_prev_size += 8 + nxt->size();
         }
         else
         {
            _size = -_size; // we are now the end.
         }
         return this;
      }

      // merge this block with the prev, return the head of new block
      block_header* merge_prev() 
      {
         auto pre = prev();
         if( !pre ) return this;
         auto _s = size() + 8;
         assert( pre->_size > 0 );
         pre->_size += _s;

         auto nxt = next();
         if( nxt ) nxt->_prev_size += _s;
         else pre->_size = -pre->_size; // 

         return pre;
      }

//   private:
      int32_t   _prev_size; // size of previous header.
      int32_t   _size; // offset to next, negitive indicates tail
      char      _data; // start of data... (ASSUME PACKED HEADER)
};

/** returns a new block page allocated via mmap 
 *  The page has 2 block headers (head+tail) defined
 *  and head is returned.
 **/
block_header* allocate_block_page();

struct block_list_node
{
    block_list_node():next(nullptr){};
    block_list_node* next;
};


class thread_allocator
{
  public:
    char*   alloc( size_t s );

    void    free( char* c )
    {
        block_header* b = reinterpret_cast<block_header*>(c) - 1;

        if( cache(b) ) return;

        auto node = reinterpret_cast<block_list_node*>(c); // store a point
        node->next = _gc_on_deck.next;
        if( !_gc_at_bat.next )
        {
           _gc_at_bat.next = node;
           _gc_on_deck.next = nullptr;
        }
        else
        {
           _gc_on_deck.next = node;
        }
    }

    static thread_allocator& get()
    {
        static __thread thread_allocator* tld = nullptr;
        if( !tld )  // new is not an option
        { 
            tld = reinterpret_cast<thread_allocator*>( malloc(sizeof(thread_allocator))/*mmap_alloc( sizeof(thread_allocator)*/ );
            tld = new (tld) thread_allocator(); // inplace construction

            // TODO: allocate  pthread_threadlocal var, attach a destructor /clean up callback
            //       to that variable... 
        }
        return *tld;
    }

  protected:
    /**
     *  dynamic cache size based on usage makes
     *  this a 'tricky' method.  Attempt to cache
     *  'h' and return true if successful.  Successful
     *  caching reduces contention on the
     *  atomic add.
     */
    bool  cache( block_header* h ) { return false; }

    thread_allocator();
    ~thread_allocator();

    friend class garbage_collector;
    bool            _done;       // cleanup and remove from list.
    block_list_node _gc_at_bat;  // where the gc pulls from.
    uint64_t        _gc_pad[7];  // gc thread and this thread should not false-share these values
    block_list_node _gc_on_deck; // where we save frees while waiting on gc to bat.

    /** 
     * called by gc thread and pops the at-bat free list
     */
    block_list_node*  get_garbage() // grab a pointer previously claimed.
    {
      if( block_list_node* gar = _gc_at_bat.next )
      {
         _gc_at_bat.next = nullptr;
         return gar;
      }
      return nullptr;
    }
    block_list_node             _bin_cache[NUM_BINS];      // head of cache for specific bin
    uint16_t                    _bin_cache_size[NUM_BINS]; // track num of nodes in cache

    thread_allocator*           _next; // used by gc to link thread_allocs together
};


typedef thread_allocator* thread_alloc_ptr;


/**
 *   Polls all threads for freed items.
 *   Upon receiving a freed item, it will look
 *   at its size and move it to the proper recycle
 *   bin for other threads to consume.
 *
 *   When there is less work to do, the garbage collector
 *   will attempt to combine blocks into larger blocks
 *   and move them to larger cache sizes until it
 *   ultimately 'completes a page' and returns it to
 *   the system.  
 *
 *   From the perspective of the 'system' an alloc
 *   involves a single atomic fetch_add.
 *
 *   A free involves a non-atomic store.
 *
 *   No other sync is necessary.
 */
class garbage_collector
{
  public:
    garbage_collector();
    ~garbage_collector();

    class recycle_bin
    {
       public:
          recycle_bin()
          :_read_pos(0),_full_count(0),_size_limit(1),write_pos(0)
          {
             memset( &_free_queue, 0, sizeof(_free_queue) );
          }

          // read the _read_pos without any atomic sync, we only care about an estimate
          int64_t available()                            { return _write_pos - *((int64_t*)&rb->_read_pos); }
          // reserve right to read the next num spots from buffer
          int64_t claim( int64_t num )                   { return _read_pos.fetch_add(num, std::memory_order_relaxed); }
          block_header* get_block( int64_t claim_pos )   { return _free_queue.at(claim_pos); }
          void          clear_block( int64_t claim_pos ) { _free_queue.at(claim_pos) = nullptr; }





          ring_buffer<block_header*,1024*256>   _free_queue; 
          std::atomic<int64_t>                  _read_pos; //written to by read threads
          int64_t _pad[7];     // below this point is written to by gc thread
          int64_t _full_count; // how many times gc thread checked and found the queue full
          int64_t _size_limit; // limit the number of blocks kept in queue
          int64_t _write_pos;  // read by consumers to know the last valid entry.

          // used to find blocks available for merging.
          std::unordered_set<block_header*>     _free_set; 
    };

    int get_bin_num( size_t s )
    {
      return LOG2(s)+1;
    }

    recycle_bin&  get_bin( size_t bin_num ) 
    { 
        assert( bin_num < NUM_BINS );
        return _bins[bin_num];
    }

    void register_allocator( thread_alloc_ptr ta );
    void unregister_allocator( thread_alloc_ptr ta );

    static garbage_collector& get()
    {
        static garbage_collector gc;
        return gc;
    }
  private:
    static void  run();
    void  recycle( char* c );

    std::thread                _thread; // gc thread.. doing the hard work
    recycle_bin                _bins[NUM_BINS];
    
    // used to track thread-local storage
    std::atomic<uint32_t>      _next_new_talloc;
    std::atomic<uint32_t>      _next_free_talloc;

    // threads that we are actively looping on
    size_t                     _tallocs_size;
    thread_alloc_ptr           _tallocs[MAX_THREADS];

    // threads that are ready for GC...
    ring_buffer<thread_alloc_ptr,MAX_THREADS> _free_tallocs;
    // threads ready to start..
    ring_buffer<thread_alloc_ptr,MAX_THREADS> _new_tallocs;
    static std::atomic<bool>   _done;
};
std::atomic<bool> garbage_collector::_done(false);

garbage_collector::garbage_collector()
:_thread( &garbage_collector::run )
{
  memset( _tallocs, 0, sizeof(_tallocs) );
}
garbage_collector::~garbage_collector()
{
  _done.store(true, std::memory_order_release );
  _thread.join();
}

void garbage_collector::register_allocator( thread_alloc_ptr ta )
{
  printf( "registering thread allocator %p\n", ta );
  // TODO: just lock here... 
  auto pos = _next_talloc.fetch_add(1);
  _tallocs[pos] = ta;
}
void garbage_collector::unregister_allocator( thread_alloc_ptr ta )
{
  for( int i = 0; i < 128; ++i )
  {
    if( _tallocs[i] == ta ) 
    {
      _tallocs[i] = nullptr;
    }
  }
}

void  garbage_collector::run()
{
    garbage_collector& self = garbage_collector::get();
    while( true )
    {
        bool found_work = false;
        for( int i = 0; i < 128; i++ )
        {
             // TODO: not safe assumption, threads can come/go at will
             // leaving holes... thread cleanup code needs locks around it
             // to prevent holes..
            if( self._tallocs[i] != nullptr ) 
            {
                auto b = self._tallocs[i]->_gc_begin;
                auto e = self._tallocs[i]->_gc_read_end;

                if( b != e ) found_work = true;
                for( auto p = b; p < e; ++p )
                {
                    char* c = self._tallocs[i]->get_garbage(p);


                    self.recycle( c);
                }
                self._tallocs[i]->_gc_begin = e; 
            }
        }
        if( !found_work ) 
        {
        //  usleep(0);
            if( _done.load( std::memory_order_acquire ) ) return;
        }
    }
}

void garbage_collector::recycle( char* c )
{
   block_header* h = ((block_header*)c)-1;
   assert( h->_next - h->_page_pos > 0 );
   recycle_bin& b = get_bin( get_bin_num(h->_next - h->_page_pos)  );
   auto p = b._next_write++;
   while( b._free_bin.at(p) != nullptr )
   {
//      fprintf( stderr, "opps.. someone left something behind...\n" );
      p = b._next_write++;
   }
   b._free_bin.at(p) = c;
   b._write_pos = p;
//   if( b._write_pos % 256 == 128 ) 
 //     b.sync_write_pos();
}

block_header* allocate_block_page()
{
    fprintf( stderr, "#" );
    auto limit = malloc(PAGE_SIZE);//mmap_alloc( PAGE_SIZE );

    block_header* bl = reinterpret_cast<block_header*>(limit);
    bl->_prev_size = 0;
    bl->_size = - (PAGE_SIZE-8);
    return _bl;
}

thread_allocator::thread_allocator()
{
  _next            = nullptr;
  _alloc_block     = nullptr;
  _gc_at_bat.next  = nullptr;
  _gc_on_deck.next = nullptr;
  garbage_collector::get().register_allocator(this);
}

thread_allocator::~thread_allocator()
{
  // give the rest of our allocated chunks to the gc thread
  // free all cache, free _alloc_block
  _done = true;
  garbage_collector::get().unregister_allocator(this);
}

char* thread_allocator::alloc( size_t s )
{
    if( s == 0 ) return nullptr;
    // calculate min block size
    s = 32*((s + 31)/32); // multiples of 64 bytes

    // if greater than PAGE_SIZE mmap_alloc
    if( s > (PAGE_SIZE-8) )
    {
       auto limit = malloc(PAGE_SIZE);//mmap_alloc( PAGE_SIZE );
       block_header* bl = reinterpret_cast<block_header*>(limit);
       bl->_prev_size = -s; // PAGE ALLOCATED blocks have a negitive prev.
       bl->_size = 0; // no forward size... 'this is the end' if this block
                      // somehow gets mixed in with others.
       return bl->data();
    }

    for( int bin = get_min_bin(s); bin < max_bin; ++bin )
    {
        block_header* b = fetch_block_from_bin(bin);
        if( b )
        {
           block_header* tail = b->split_after( s );
           if( tail ) free( tail->data() );
           return b->data();
        }
    }

    block_header* new_page = allocate_block_page();
    block_header* tail = new_page->split_after(s);
    if( tail ) free( tail->data() );
    return new_page->data();
}

/**
 *  Checks our local bin first, then checks the global bin.
 *
 *  @return null if no block found in cache.
 */
block_header* thread_allocator::fetch_block_from_bin( int bin )
{
    if( _bin_cache[bin].next ) 
    {
        _bin_cache_size[bin]--;
        block_header* bh = _bin_cache[bin].next;
        _bin_cache[bin].next = reinterpret_cast<block_list_node*>(bh->data)->next;
        return bh;
    }
    else
    {
        garbage_collector& gc              = garbage_collector::get();
        garbage_collector::recycle_bin& rb = gc.get_bin( bin );

        if( auto avail = rb.available()  )
        {
            // claim up to half of the available, just incase 2
            // threads try to claim at once, they both can, but
            // don't hold a cache of more than 4 items
            auto claim_num = std::min( avail/2, 3 ); 
            // claim_num could now be 0 to 3
            claim_num++; // claim at least 1 and at most 4

            // this is our one and only atomic 'sync' operation... 
            auto claim_pos = rb.claim( claim_num );
            auto claim_end = claim_pos + claim_num;
            bool found = false;
            while( claim_pos != claim_end )
            {
               block_header* h = rb.get_block(claim_pos);
               if( h )
               {
                  found = true;
                  rb.clear_block(claim_pos); // let gc know we took it. 
                  ln = reinterpret_cast<block_list_node*>(h->data());
                  ln->next = _bin_cache[bin]->next;
                  _bin_cache[bin]->next = ln;
               }
               else // oops... I guess 3 tried to claim at once...
               {
                  // drop it on the floor and let the
                  // gc thread pick it up next time through the
                  // ring buffer.
               }
               ++claim_pos;
            }
            if( found ) 
               return fetch_block_from_bin(bin); // grab it from the cache this time.
        }
    }
    return nullptr;
}

char* malloc2( int s )
{
  return thread_allocator::get().alloc(s);
}

void  free2( char* s )
{
  return thread_allocator::get().free(s);
}


/*  SEQUENTIAL BENCH
int main( int argc, char** argv )
{
  if( argc == 2 && argv[1][0] == 'S' )
  {
     printf( "malloc2\n");
     for( int i = 0; i < 50000000; ++i )
     {
        char* test = malloc2( 128 );
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
std::vector<char*>  buffers[16];
void pc_bench_worker( int pro, int con, char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  for( int r = 0; r < ROUNDS; ++r )
  {
      for( int x = 0; x < buffers[pro].size()/2 ; ++x )
      {
         uint32_t p = fast_rand() % buffers[pro].size();
         if( !buffers[pro][p] )
         {
           uint64_t si = 32 + fast_rand()%(8096*16); //4000;//32 + fast_rand() % (1<<16);
           auto r = do_alloc( si );
           assert( r != nullptr );
         //  assert( r[0] != 99 ); 
         //  r[0] = 99; 
           buffers[pro][p] = r;
         }
      }
      for( int x = 0; x < buffers[con].size()/2 ; ++x )
      {
         uint32_t p = fast_rand() % buffers[con].size();
         assert( p < buffers[con].size() );
         assert( con < 16 );
         assert( con >= 0 );
         if( buffers[con][p] ) 
         { 
           //assert( buffers[con][p][0] == 99 ); 
          // buffers[con][p][0] = 0; 
           do_free(buffers[con][p]);
           buffers[con][p] = 0;
         }
      }
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
  int i = 0;
  std::thread a( [=](){ pc_bench_worker( 1, 1, do_alloc, do_free ); } );
  a.join();
}
#include <tbb/scalable_allocator.h>

char* do_malloc(int s)
{ 
//    return (char*)::malloc(s); 
   return (char*)scalable_malloc(s);
}
void  do_malloc_free(char* c)
{ 
    scalable_free(c);
  // ::free(c); 
}

int main( int argc, char** argv )
{
  if( argc > 2 && argv[1][0] == 'm' )
  {
    std::cerr<<"malloc multi\n";
    pc_bench( atoi(argv[2]), do_malloc, do_malloc_free );
  }
  if( argc > 2 && argv[1][0] == 'M' )
  {
    std::cerr<<"hash malloc multi\n";
    pc_bench( atoi(argv[2]), malloc2, free2 );
  }
  if( argc > 1 && argv[1][0] == 's' )
  {
    std::cerr<<"malloc single\n";
    pc_bench_st( do_malloc, do_malloc_free );
  }
  if( argc > 1 && argv[1][0] == 'S' )
  {
    std::cerr<<"hash malloc single\n";
    pc_bench_st( malloc2, free2 );
  }
  return 0;
}







