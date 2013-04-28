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
#include "mmap_alloc.hpp"
#include "disruptor.hpp"
#include <thread>
#include "fast_rand.cpp"

using namespace disruptor;

#define PAGE_SIZE (8*1024*1024)
#define BENCH_SIZE ( (1024*512) )
#define ROUNDS 30 

struct block_header
{
   uint32_t   _page_pos; // how far from start of page
   uint32_t   _prev;
   uint32_t   _next;
   uint32_t   _timestamp;// creation time... we want to use 'old blocks' first
                         // because they are most likley to contain long-lived objects
};
block_header* allocate_block_page();

/**
 *  2MB chunk of memory that gets divided up
 *  'on request', rounded to the nearest multiple
 *  of 128 bytes so that it can be binned/cached
 *  effectively.
 */
struct page
{
  block_header   data[PAGE_SIZE/sizeof(block_header)]; 
};

class thread_allocator
{
  public:
    void    free( char* c )
    {
      auto pos = _gc_read_end_buffer;
      _garbage_bin.at(pos) = c;
      _gc_read_end_buffer = pos + 1;
      /*
      _gc_read_end_buffer = pos + 1;
      */
      if( _gc_read_end_buffer - _gc_read_end_last_write > 10 )
      {
        _gc_read_end = _gc_read_end_last_write = _gc_read_end_buffer;
      }
    }

    char*   alloc( size_t s );

    static thread_allocator& get()
    {
        static __thread thread_allocator* tld = nullptr;
        if( !tld )  // new is not an option
        { 
            tld = reinterpret_cast<thread_allocator*>( mmap_alloc( sizeof(thread_allocator) ) );
            tld = new (tld) thread_allocator(); // inplace construction

            // TODO: allocate  pthread_threadlocal var, attach a destructor /clean up callback
            //       to that variable... 
        }
        return *tld;
    }

  protected:
    thread_allocator();
    ~thread_allocator();

    friend class garbage_collector;
    
    int64_t           _gc_begin;               // how far has gc processed
    int64_t           _pad[7];                 // save the cache lines/prevent false sharing
    volatile  int64_t _gc_read_end;            // how far can gc read
    int64_t           _pad2[7];                // save the cache lines/prevent false sharing
    int64_t           _gc_read_end_buffer;     // cache writes to gc_read_end to every 10 writes
    int64_t           _gc_read_end_last_write; // cache writes to gc_read_end to every 10 writes
    int64_t           _cache_pos;
    int64_t           _cache_end;

    char*   get_garbage( int64_t pos ) // grab a pointer previously claimed.
    {
      // we may have to dynamically reallocate our gbin
      return _garbage_bin.at(pos);
    }
    block_header*               _next_block;
    ring_buffer<char*,1024*16>  _garbage_bin;
    ring_buffer<char*,64>       _cache;
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
    /**
     *  Handles objects of the same size.
     */
    class recycle_bin
    {
       public:
          recycle_bin()
          :_next_write(0),_write_pos(0),_read_pos(0)
          {
          }
          int64_t                       _next_write;
          int64_t                       _pad0[7];
          int64_t                       _write_pos;
          int64_t                       _pad[7];
          std::atomic<int64_t>          _read_pos;
          int64_t                       _pad2[7];
          ring_buffer<char*,1024*1024>  _free_bin;
    };

    recycle_bin&  get_bin( size_t s ) { return _bins[0]; }

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

    std::thread            _thread;
    recycle_bin            _bins[1];
    std::atomic<uint32_t>  _next_talloc;
    thread_alloc_ptr       _tallocs[128];
    static std::atomic<bool>      _done;
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
   //printf( "recycle thread allocator page_pos: %d\n", h->_page_pos );
   switch( h->_next - h->_page_pos )
   {
      default:
      {
        recycle_bin& b = _bins[0];
        auto p = b._next_write++;
        b._free_bin.at(p) = c;
     //   b._write_cur.publish(p);
        b._write_pos = p;
      }
   }
}

block_header* allocate_block_page()
{
    printf( "allocate block page!\n" );
    auto limit = mmap_alloc( PAGE_SIZE );

    block_header* _next_block = reinterpret_cast<block_header*>(limit);
    _next_block->_page_pos = 0;
    _next_block->_prev = 0;
    _next_block->_next = PAGE_SIZE; // next block always goes to end...; 
    _next_block->_timestamp = 0; // TODO... 
    return _next_block;
}


thread_allocator::thread_allocator()
{
  _gc_begin = 0;
  _gc_read_end = 0;
  _gc_read_end_buffer = 0;
  _gc_read_end_last_write = 0;
  _next_block = allocate_block_page();
  _cache_pos = 0;
  _cache_end = 0;

  garbage_collector::get().register_allocator(this);
}

thread_allocator::~thread_allocator()
{
  // give the rest of our allocated chunks to the gc thread
  free( reinterpret_cast<char*>(_next_block+1) ); 
  garbage_collector::get().unregister_allocator(this);

  // GARBAGE COLLECTOR must do the mmap free because we don't know
  // when it will notice this thread going away... 
  // TODO: post a message to GC to track thread cleanup.
  
  // mmap_free( this, sizeof(*this) );
}


char* thread_allocator::alloc( size_t s )
{
    s = 64*((s + 63)/64); // multiples of 64 bytes

    if( s+sizeof(block_header) >= PAGE_SIZE  )
    {
       assert( false );
       // do direct mmap 
      return nullptr;
    }
    if( _cache_pos < _cache_end )
    {
       char* c = _cache.at(_cache_pos);
       ++_cache_pos;
       return c;
    }

    garbage_collector::recycle_bin* rb = &garbage_collector::get().get_bin( s );
    while( rb )
    {
       // TODO: ATOMIC ... switch to non-atomic check
       auto write_pos = rb->_write_pos;
      // printf( "recyclebin wirte_pos: %d  read_cur.begin %d\n", write_pos, rb->_read_cur.pos().aquire()  );

       auto avail = write_pos - *((int64_t*)&rb->_read_pos);
       if(  avail > 32 )// /*.load( std::memory_order_relaxed )*/ < write_pos )
       {
          // ATOMIC CLAIM FROM SHARED POOL... MOST EXPENSIVE OP WE HAVE...
          //auto pos = rb->_read_cur.pos().atomic_increment_and_get(1)-1;
          auto pos = rb->_read_pos.fetch_add(16,std::memory_order_relaxed);
          auto e = pos + 16;
          while( pos < e )
          {
             char* b = rb->_free_bin.at(pos);
             if( b )
             {
                _cache.at(_cache_end++) = b;
                rb->_free_bin.at(pos) = nullptr;
             }
             ++pos;
          }

          if( _cache_pos < _cache_end )
          {
             char* c = _cache.at(_cache_pos);
             ++_cache_pos;
             return c;
          }
       } // else there are no blocks our size... go up a size or two?..
       break;
    }
    // we already checked the 'best fit' bin and failed to find 
    // anything that size ready, so we can allocate it from our 
    // thread local block

 //   printf( "allocating new chunk from thread local page\n" );

    // make sure the thread local block has enough space...
    if( _next_block->_page_pos + s + sizeof(block_header) > PAGE_SIZE )
    {
        // not enough space left in current block.. free it... if it has any space at all.
        if( _next_block->_page_pos != PAGE_SIZE )
            free( (char*)(_next_block+1) );

        _next_block = allocate_block_page();
    }

    block_header* new_b   = _next_block;
    _next_block = new_b + 1 + s/sizeof(block_header);

    _next_block->_page_pos  = new_b->_page_pos + sizeof(block_header) + s;
    _next_block->_prev      = new_b->_page_pos; 
    _next_block->_next      = PAGE_SIZE; // next block always goes to end...
    _next_block->_timestamp = new_b->_timestamp; // TODO...

    new_b->_next            = _next_block->_page_pos;
    
    // our work here is done give them the newly allocated block (pointing after the header
    return reinterpret_cast<char*>(new_b+1);
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
           auto si = 60; //fast_rand() % (1<<15);
           auto r = do_alloc( si );
         //  assert( r != nullptr );
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


void pc_bench(char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  for( int i = 0; i < 16; ++i )
  {
    buffers[i].resize( BENCH_SIZE );
    memset( buffers[i].data(), 0, 8 * BENCH_SIZE );
  }
  std::thread a( [=](){ pc_bench_worker( 1, 2, do_alloc, do_free ); } );
  std::thread b( [=](){ pc_bench_worker( 2, 3, do_alloc, do_free ); } );
  std::thread c( [=](){ pc_bench_worker( 3, 4, do_alloc, do_free ); } );
  std::thread d( [=](){ pc_bench_worker( 4, 5, do_alloc, do_free ); } );
  std::thread e( [=](){ pc_bench_worker( 5, 6, do_alloc, do_free ); } );
  std::thread f( [=](){ pc_bench_worker( 6, 7, do_alloc, do_free ); } );
  std::thread g( [=](){ pc_bench_worker( 7, 8, do_alloc, do_free ); } );
  std::thread h( [=](){ pc_bench_worker( 8, 9, do_alloc, do_free ); } );
  std::thread i( [=](){ pc_bench_worker( 9, 10, do_alloc, do_free ); } );
  std::thread j( [=](){ pc_bench_worker( 10, 1, do_alloc, do_free ); } );

  a.join();
  b.join();
  c.join();
  d.join();
  e.join();
  f.join();
  g.join();
  h.join();
  i.join();
  j.join();
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


char* do_malloc(int s){ return (char*)::malloc(s); }
void  do_malloc_free(char* c){ ::free(c); }

int main( int argc, char** argv )
{
  if( argc > 1 && argv[1][0] == 'm' )
  {
    std::cerr<<"malloc multi\n";
    pc_bench( do_malloc, do_malloc_free );
  }
  if( argc > 1 && argv[1][0] == 'M' )
  {
    std::cerr<<"hash malloc multi\n";
    pc_bench( malloc2, free2 );
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







