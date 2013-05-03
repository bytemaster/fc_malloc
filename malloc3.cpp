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

#include "mmap_alloc.hpp"
#include "disruptor.hpp"
#include <thread>
#include <stdint.h>
#include <memory.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <assert.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
//#include "rand.cpp"


using namespace disruptor;

#define PAGE_SIZE (4*1024*1024)
#define BENCH_SIZE ( (1024) )
#define ROUNDS 200000
#define LOG2(X) ((unsigned) (8*sizeof (unsigned long long) - __builtin_clzll((X)) - 1))
#define NUM_BINS 32 // log2(PAGE_SIZE)

class block_header
{
   public:
      block_header* next()
      { 
         assert(this);
         if( _size > 0 ) return reinterpret_cast<block_header*>(data()+_size); 
         else return nullptr;
      }
      block_header* prev()
      { 
         assert(this);
         if( _prev_size <= 0 ) return nullptr;
         return reinterpret_cast<block_header*>(reinterpret_cast<char*>(this) - _prev_size - 8);
      }

      enum flags_enum
      {
         unknown  = 0,
         idle     = 1, // in storage, mergable
         queued   = 2, // in waiting queue...
         cached   = 4, // cached in thread
         active   = 8, // in use by app
         mergable = 16 // track this or will false sharing kill me?
      };

      struct queue_state // the block is serving as a linked-list node
      {
          block_header* next;
          block_header* prev;
      };

      void set_state( flags_enum e )
      {
         _flags = e;
      }
      flags_enum get_state() { return (flags_enum)_flags; }

      queue_state& as_queue_node()
      {
         return *reinterpret_cast<queue_state*>(data());
      }

      queue_state& init_as_queue_node()
      {
         // _flags |= queued;
         queue_state& s = as_queue_node();
         s.next = nullptr;
         s.prev = nullptr;
         return s;
      }


      void init( int s )
      {
         _prev_size = 0;
         _size = - (s-8);
      }

      char*         data()      { return ((char*)this)+8; }
      int           size()const { return abs(_size); }

      int raw_size()const { return _size; }
      int raw_prev_size()const { return _prev_size; }


      int        calc_forward_extent()
      {
         // fprintf( stderr, "pos %p + %d  -> ", this, _size );
          int s = size() + 8;
          auto n = next();
          if( n ) s += n->calc_forward_extent();
          return s;
      }

      int       page_size()
      {
          auto h = head();
          assert(h);
          return head()->calc_forward_extent(); 
      }
      block_header*       head()
      {
          auto pre = prev();
          if( !pre ) return this;
          do {
            auto next_prev = pre->prev();
            if( !next_prev ) return pre;
            pre = next_prev;
          } while ( true );
      }

      /** create a new block at p and return it */
      block_header* split_after( int s )
      {
         assert( s >= 32 );
//         fprintf( stderr, "prev_size %d  _size %d  Initial Error: %d\n", _prev_size, _size, int(PAGE_SIZE - this->page_size()) );
         assert( PAGE_SIZE == page_size() );
         
         if(  (size() - 8 -32) < s ) return nullptr;// no point in splitting to less than 32 bytes

         block_header* n = reinterpret_cast<block_header*>(data()+s);
         n->_prev_size   = s;
         n->_size        = size() -s -8;
         
         if( _size < 0 ) 
            n->_size = -n->_size; // we just split the tail

         _size = s; // this node now has size s
         assert( size() >= s );
         assert( PAGE_SIZE == n->page_size() );
         assert( PAGE_SIZE == page_size() );
         return n;
      }

      // merge this block with next, return head of new block.
      block_header* merge_next()
      {
         assert( PAGE_SIZE == page_size() );
         assert( _flags == block_header::idle );
         auto nxt = next();
         if( !nxt ) return this;
         assert( nxt->page_size() == PAGE_SIZE );

         // next must be in the idle state
         if( nxt->_flags != idle ) return this;

         // extract node from the double link list it is in.
         queue_state& qs = nxt->as_queue_node();
         if( qs.next )
         {
      //      assert( qs.next->as_queue_node().prev == nxt );
            qs.next->as_queue_node().prev = qs.prev;
         }

         if( qs.prev )
         {
       //     assert( qs.prev->as_queue_node().next == nxt );
            qs.prev->as_queue_node().next = qs.next;
         }

         // now we are free to merge the memory
         _size += nxt->size() + 8;
         fprintf( stderr, "merged to size %d\n", _size );
         if( nxt->_size < 0 ) _size = -_size;

         nxt = next(); // find the new next.
         if( nxt )
         {
           nxt->_prev_size = size();
         }
         assert( PAGE_SIZE == page_size() );
         if( next() ) assert( PAGE_SIZE == next()->page_size() );
         if( prev() ) assert( PAGE_SIZE == prev()->page_size() );
         return this;
      }

      // merge this block with the prev, return the head of new block
      block_header* merge_prev() 
      {
         _flags = idle; // mark myself as idle/mergable
         auto p = prev();
         if( !p ) return this;
         if( p->_flags != idle ) return this;
         return p->merge_next();
      }

   private:
      int32_t   _prev_size; // size of previous header.
      int32_t   _size:24; // offset to next, negitive indicates tail, 8 MB max, it could be neg
      int32_t   _flags:8; // offset to next, negitive indicates tail
};
static_assert( sizeof(block_header) == 8, "Compiler is not packing data" );

/** returns a new block page allocated via mmap 
 *  The page has 2 block headers (head+tail) defined
 *  and head is returned.
 **/
block_header* allocate_block_page();

struct block_list_node
{
    block_list_node():next(nullptr){};
    block_list_node* next;

    block_header*    header()
    {
      return  reinterpret_cast<block_header*>(reinterpret_cast<char*>(this)-8);
    }

    int count()
    {
      int count = 1;
      auto n = next;
      while( n )
      {
        ++count;
        assert( count < 1000 );
        n = n->next;
      }
      return count;
    }

    block_list_node* find_end() 
    {
       block_list_node* n = this;
       while( n->next )
       {
          n = n->next;
       }
       return n;
    }
};


class thread_allocator
{
  public:
    char*   alloc( size_t s );

    void    free( char* c )
    {
        auto node = reinterpret_cast<block_header*>(c-8); // store a point
        node->init_as_queue_node().next = _gc_on_deck;
        if( !_gc_at_bat )
        {
           _gc_at_bat = node;
           _gc_on_deck = nullptr;
        }
        else
        {
           _gc_on_deck = node;
        }
    }

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

    void print_cache()
    {
       for( int i = 0; i < NUM_BINS; ++i )
       {
          fprintf( stderr, "%d]  size %d   \n", i, _bin_cache_size[i] );
       }
    }

  protected:

    bool          store_cache( block_header* h )
    {
      assert( h->page_size() == PAGE_SIZE );
       auto bin = LOG2( h->size() );
      if( _bin_cache[bin] == nullptr )
      {
        _bin_cache[bin] = h;
        return true;
      }
      return false;
      /*
       assert( h != nullptr );

       if( _bin_cache_size[bin] < 4 )
       {
          if( _bin_cache_size[bin] == 0 ) assert( nullptr == _bin_cache[bin] );

          block_list_node* bln = reinterpret_cast<block_list_node*>(h->data() );
          bln->next = _bin_cache[bin];
          _bin_cache[bin] = bln;
          _bin_cache_size[bin]++;
          assert( _bin_cache_size[bin] == _bin_cache[bin]->count() );
          return true;
       }
       fprintf( stderr, "cache full bin %d size %d", bin, _bin_cache_size[bin] );
       assert( _bin_cache[bin] != nullptr );
       return false;
       */
    }

    block_header* fetch_cache( int bin )
    {
       if( _bin_cache[bin] )
       {
         block_header* b = _bin_cache[bin];
         assert( b->page_size() == PAGE_SIZE );
         _bin_cache[bin] = nullptr;
         return b;
       }
       return nullptr;
      /*
       if( _bin_cache_size[bin] > 0 )
       {
          assert( _bin_cache_size[bin] == _bin_cache[bin]->count() );
          assert( _bin_cache[bin] );
          auto h = _bin_cache[bin];
          _bin_cache[bin] = h->next;
          _bin_cache_size[bin]--;
          auto head = h->header();
          assert( head->page_size() == PAGE_SIZE );
          assert( LOG2(head->size()) >= bin );
          assert( LOG2(head->size()) == bin );
          return head;
       }
       assert( !_bin_cache[bin] );
       */
       return nullptr;
    }



    block_header* fetch_block_from_bin( int bin );

    thread_allocator();
    ~thread_allocator();

    friend class garbage_collector;
    bool                         _done;       // cleanup and remove from list.
    std::atomic<block_header*>   _gc_at_bat;  // where the gc pulls from.
    uint64_t                     _gc_pad[7];  // gc thread and this thread should not false-share these values
    block_header*                _gc_on_deck; // where we save frees while waiting on gc to bat.

    /** 
     * called by gc thread and pops the at-bat free list
     */
    block_header*  get_garbage() // grab a pointer previously claimed.
    {
      if( block_header* gar = _gc_at_bat.load() )
      {
         _gc_at_bat.store(nullptr);// = nullptr;
         return gar;
      }
      return nullptr;
    }
    block_header*               _bin_cache[NUM_BINS];      // head of cache for specific bin
    int16_t                     _bin_cache_size[NUM_BINS]; // track num of nodes in cache

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
          :_read_pos(0),_full_count(0),_full(2),_write_pos(0)
          {
             memset( &_free_queue, 0, sizeof(_free_queue) );
             _free_list = nullptr;
          }

          // read the _read_pos without any atomic sync, we only care about an estimate
          int64_t available()                            { return _write_pos - *((int64_t*)&_read_pos); }
          // reserve right to read the next num spots from buffer
          int64_t claim( int64_t num )                   { return _read_pos.fetch_add(num); }
          block_header* get_block( int64_t claim_pos )   { return _free_queue.at(claim_pos); }
          void          clear_block( int64_t claim_pos ) { _free_queue.at(claim_pos) = nullptr; }

          // determines how many chunks should be required to consider this bin full.
          // TODO: this method needs to be tweaked to factor in 'time'... as it stands
          // now the GC loop will be very agressive at shrinking the queue size
          int64_t       check_status()
          {
              return 8 - available();
            /*
              auto av = available();
              int consumed = _last_fill - av;
              if( consumed > _last_fill/2 ) ++_full;

              if( av <= 0 )
              {
                 // apparently there is high demand, the consumers cleaned us out.
                 _full *= 2; // exponential growth..
                 _full = std::min( _full+4, _free_queue.get_buffer_size() -1 );
                 fprintf( stderr, "%d  blocks available,   _full %d\n", int(av), int(_full) );
              }
              else if( av == _full )
              {
                 // apparently no one wanted any... we should shrink what we consider full
                 _full -= 4; // fast back off
                 if( _full < 2 ) _full = 2;
              }
              else // av < _full
              {
                 // some, but not all have been consumed... 
                 // if less than half have been consumed... reduce size,
                 // else keep the size the same.
                 if( av > _full/2 )
                 {
                     _full--; // reduce full size,slow back off
                     if( _full < 2 ) _full = 2;
                     return  _full - av; 
                 }
                 else // more than half consumed... keep full size the same, refill
                 {
                 }
              }
               fprintf( stderr, "%d  blocks available,   _full %d  post %d\n", int(av), int(_full), int(_full-av) );
              return _full - av; 
              */
          }



          ring_buffer<block_header*,128>         _free_queue; 
          std::atomic<int64_t>                  _read_pos; //written to by read threads
          int64_t _pad[7];     // below this point is written to by gc thread
          int64_t _full_count; // how many times gc thread checked and found the queue full
          int64_t _full;       // limit the number of blocks kept in queue
          int64_t _write_pos;  // read by consumers to know the last valid entry.
          int64_t _last_fill;  // status of the buffer at the last check.

          void push( block_header* h )
          {
             h->set_state( block_header::idle );
             block_header::queue_state& qs = h->init_as_queue_node(); 
             qs.next = _free_list;
             if( _free_list ) 
             {
                _free_list->as_queue_node().prev = h;
             }
             _free_list = h;
          }

          block_header* pop()
          {
              auto tmp = _free_list;
              if( _free_list ) 
              {
                 auto n = _free_list->as_queue_node().next;
                 if( n ) 
                    n->as_queue_node().prev = nullptr;
                 _free_list = n;
                 assert( tmp->get_state() == block_header::idle );
                 tmp->set_state( block_header::unknown ); // TODO: only if DEBUG
              }
              return tmp;
          }

          // blocks are stored as a double-linked list
          block_header* _free_list;
    };

    recycle_bin& find_cache_bin_for( block_header* h ) 
    { 
      assert(h!=nullptr);
      int bn = get_bin_num(h->size());
  //    fprintf( stderr,  "block header size %d  is cached in bin %d holding sizes %d\n", (int)h->size(), bn, (1<<(bn)) );
      return get_bin(get_bin_num( h->size() )); 
    }

    int get_bin_num( size_t s )
    {
      return LOG2(s);
    }

    recycle_bin&  get_bin( size_t bin_num ) 
    { 
        assert( bin_num < NUM_BINS );
        return _bins[bin_num];
    }

    void register_allocator( thread_alloc_ptr ta );

    static garbage_collector& get()
    {
        static garbage_collector gc;
        return gc;
    }
  private:
    static void  run();
    // threads that we are actively looping on
    std::atomic<thread_alloc_ptr> _thread_head;

    std::thread                _thread; // gc thread.. doing the hard work
    recycle_bin                _bins[NUM_BINS];


    static std::atomic<bool>   _done;
};
std::atomic<bool> garbage_collector::_done(false);

garbage_collector::garbage_collector()
:_thread_head(nullptr),_thread( &garbage_collector::run )
{
   fprintf( stderr, "allocating garbage collector\n" );
}
garbage_collector::~garbage_collector()
{
  _done.store(true, std::memory_order_release );
  _thread.join();
}

void garbage_collector::register_allocator( thread_alloc_ptr ta )
{
  printf( "registering thread allocator %p\n", ta );

  auto* stale_head = _thread_head.load(std::memory_order_relaxed);
  do { ta->_next = stale_head;
  }while( !_thread_head.compare_exchange_weak( stale_head, ta, std::memory_order_release ) );
}

void  garbage_collector::run()
{
    fprintf( stderr, "Starting GC loop\n");
    try
    {
      garbage_collector& self = garbage_collector::get();
      while( true )
      {
          thread_alloc_ptr cur_al = *((thread_alloc_ptr*)&self._thread_head);
          bool found_work = false;
      
          // for each thread, grab all of the free chunks and move them into
          // the proper free set bin, but save the list for a follow-up merge
          // that takes into consideration all free chunks.
          while( cur_al )
          {
              auto cur = cur_al->get_garbage();
              
              if( cur )
              {
                assert( cur->page_size() == PAGE_SIZE );
                found_work = true; 
              }
              
              while( cur )
              {
                  assert( cur->page_size() == PAGE_SIZE );
                  block_header* nxt = cur->as_queue_node().next;
                  assert( nxt != cur );
                  if( nxt ) assert( nxt->page_size() == PAGE_SIZE );

                  assert( cur->page_size() == PAGE_SIZE );
                  auto before = cur->size();
                //  fprintf( stderr, "found free block of size: %d\n", cur->size() );
                  cur->init_as_queue_node();
                  assert( cur->page_size() == PAGE_SIZE );
                  cur->set_state( block_header::idle );
                  assert( cur->page_size() == PAGE_SIZE );

                  cur = cur->merge_next();
               //   cur = cur->merge_prev();
                  if( before != cur->size() )
                  fprintf( stderr, "found free block of after merges..: %d\n", cur->size() );
              
                  assert( cur->page_size() == PAGE_SIZE );
                  recycle_bin& c_bin = self.find_cache_bin_for(cur);
                  assert( cur->page_size() == PAGE_SIZE );
              //    fprintf( stderr, "pushing into bin\n" );
                  c_bin.push(cur); 
                  assert( cur->page_size() == PAGE_SIZE );
              
                  cur = nxt;
                  assert( cur->page_size() == PAGE_SIZE );
              }

              assert( cur_al != cur_al->_next );
              // get the next thread.
              cur_al = cur_al->_next;
          }
      
          // for each recycle bin, check the queue to see if it
          // is getting low and if so, put some chunks in play
          for( int i = 0; i < NUM_BINS; ++i )
          {
              garbage_collector::recycle_bin& bin = self._bins[i];
              auto needed = bin.check_status(); // returns the number of chunks need
              if( needed > 0 )
              {
                  int64_t next_write_pos = bin._write_pos;
                  block_header* next = bin.pop();

                  while( next && needed > 0 )
                  {
                   //   fprintf( stderr, "poping block from bin %d and pushing into queue\n", i );
                      found_work = true;
                      ++next_write_pos;
                      if( bin._free_queue.at(next_write_pos) )
                      {
                          // someone left something behind... 
                      }
                      else
                      {
                          bin._free_queue.at(next_write_pos) = next;
                          next = bin.pop();
                      }
                      --needed;
                  }
                  if( next ) bin.push(next); // leftover... 
                  bin._write_pos = next_write_pos;
              }
              else if( needed < 0 )
              {
                // apparently no one is checking this size class anymore, we can reclaim some nodes.
                // TODO:  perhaps we only do this if there is no other work found as work implies
                // that the user is still allocating / freeing objects and thus we don't want to
                // compete to start freeing cache yet... 
              }
          }
          if( !found_work ) usleep( 1000 );
      
          if( _done.load( std::memory_order_acquire ) ) return;
          if( !found_work ) 
          {
              // reclaim cache
              // sort... and optimize....
          }
      }
    }
    catch ( ... )
    {
        fprintf( stderr, "gc caught exception\n" );
    }
    fprintf( stderr, "exiting gc loop\n" );
}


block_header* allocate_block_page()
{
    fprintf( stderr, "\n\n                                                                               ALLOCATING NEW PAGE\n\n" );
    auto limit = mmap_alloc( PAGE_SIZE );

    block_header* bl = reinterpret_cast<block_header*>(limit);
    bl->init( PAGE_SIZE );
    
    return bl;
}

thread_allocator::thread_allocator()
{
  _done            = false;
  _next            = nullptr;
  //_gc_at_bat       = nullptr;
  _gc_on_deck      = nullptr;

  memset( _bin_cache, 0, sizeof(_bin_cache) );
  memset( _bin_cache_size, 0, sizeof(_bin_cache_size) );
  garbage_collector::get().register_allocator(this);
}

thread_allocator::~thread_allocator()
{
  // give the rest of our allocated chunks to the gc thread
  // free all cache, free _alloc_block
  _done = true;
}

int get_min_bin( size_t s )
{
  return LOG2(s)+1;
}

char* thread_allocator::alloc( size_t s )
{
 //   fprintf( stderr, "    alloc %d\n", (int)s );
    if( s == 0 ) return nullptr;
    size_t data_size = s;

    // we need 8 bytes for the header, then round to the nearest
    // power of 2.
    int min_bin = LOG2(s+7)+1; // this is the bin size.
    s = (1<<min_bin)-8; // the data size is bin size - 8
    assert( s >= data_size );
    
    for( int bin = min_bin; bin < NUM_BINS; ++bin )
    {
        block_header* b = fetch_block_from_bin(bin);
        if( b )
        {
           fprintf( stderr, "found cache in bin %d\r", bin );
           assert( b->page_size() == PAGE_SIZE );
           block_header* tail = b->split_after( s );
           assert( b->page_size() == PAGE_SIZE );
           if( tail ) assert( tail->page_size() == PAGE_SIZE );
           assert( b->size() >= s );
           if( tail && !store_cache( tail ) ) 
           {
              fprintf( stderr, "unable to cache tail, free it\n" );
              this->free( tail->data() );
           }
           assert( b->size() >= s );
           return b->data();
        }
    }


    block_header* new_page = allocate_block_page();
    //printf( "      alloc new block page   %p  _size  %d _prev_size %d  next %p  prev %p\n",
      //    new_page, new_page->_size, new_page->_prev_size, new_page->next(), new_page->prev() );
    block_header* tail = new_page->split_after(s);
//    printf( "      alloc free tail  %p  _size  %d _prev_size %d  next %p  prev %p  tail %p\n",
 //         tail, tail->_size, tail->_prev_size, tail->next(), tail->prev(), tail );
    
    if( tail && !store_cache( tail ) )
    {
       this->free( tail->data() );
    }

    assert( new_page->size() >= s-8 );
    return new_page->data();
}

/**
 *  Checks our local bin first, then checks the global bin.
 *
 *  @return null if no block found in cache.
 */
block_header* thread_allocator::fetch_block_from_bin( int bin )
{
//    fprintf( stderr, "fetch cache %d  has %d items remaining\n", bin, int(_bin_cache_size[bin]) );    
    auto lo = fetch_cache(bin);
    if( lo ) return lo;
    assert( _bin_cache_size[bin] == 0 );

    garbage_collector& gc              = garbage_collector::get();
    garbage_collector::recycle_bin& rb = gc.get_bin( bin );

    if( auto avail = rb.available()  )
    {
        // claim up to half of the available, just incase 2
        // threads try to claim at once, they both can, but
        // don't hold a cache of more than 4 items
        auto claim_num = 2;//std::min<int64_t>( avail/2, 1 ); 
        // claim_num could now be 0 to 3
        //claim_num++; // claim at least 1 and at most 4

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
              ++claim_pos;
              if( claim_pos == claim_end )
              {
                  return h;
              }
              else if( !store_cache(h ) )
              {
                assert( !"unable to cache something we asked for!"  );
              }
           }
           else // oops... I guess 3 tried to claim at once...
           {
              ++claim_pos;
              // drop it on the floor and let the
              // gc thread pick it up next time through the
              // ring buffer.
           }
        }
        if( found ) 
        {
           fprintf( stderr, "apparently we were over drew the queue...\n" );
           return fetch_cache(bin); // grab it from the cache this time.
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


#include "bench.cpp"
