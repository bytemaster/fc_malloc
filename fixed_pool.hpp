#include <thread>
#include <atomic>
#include "mmap_alloc.hpp"
#include "bit_index.hpp"

#define GB (1024LL*1014LL*1024LL)
#define MB (1024LL*1024LL)
#define LOG2(X) ((unsigned) (8*sizeof (unsigned long long) - __builtin_clzll((X)) - 1))

class basic_page
{
  public:
    virtual ~basic_page(){}
    virtual void  release() = 0;
    virtual void* alloc() = 0;
    virtual bool  free( void* ) = 0;
 //   virtual void  item_size()const = 0;
};

class basic_pool
{
  public:
    virtual ~basic_pool(){}
    virtual basic_page* claim_page() = 0;
    virtual bool  gc_free(void*) = 0;
};

typedef basic_pool* basic_pool_ptr;
typedef basic_page* basic_page_ptr;

template<uint64_t ItemSize, uint64_t PageSize = 2*MB>
class fixed_pool : public basic_pool
{
  public:
    class page : public basic_page
    {
       public:
          
          page( int64_t claim_pos )
          {
            fprintf( stderr, "CLAIM POS %lld\n", claim_pos );
            _free_blocks.clear_all();
            _stage_blocks.clear_all();
            _alloc_blocks.set_all();
            _avail = PageSize / ItemSize;
            _data = (char*)mmap_alloc( PageSize, (void*)((ItemSize << 32) + claim_pos * PageSize) );
            fprintf( stderr, " PAGE DATA: %p\n", _data );
            assert( (int64_t(_data) >> 32) == ItemSize );
          }
          ~page()
          {
            mmap_free( _data, PageSize );
          }

          void update_avail_list()
          {
              // check the free bits list to see if there is anything new.
              auto gb        = _free_blocks.first_set_bit();

              // if nothing in the free list, check the stage list
              if( gb >= _free_blocks.size() )
                  gb         = _stage_blocks.first_set_bit();

              // if there are any free bits in the free/stage list then
              // update the alloc/stage bits
              if( gb < _free_blocks.size() )
              {
                 auto gb_itr     = _free_blocks.at(gb);
                 auto stage_itr  = _stage_blocks.at(gb);
                 auto a_itr      = _alloc_blocks.at(gb);
                 
                 uint64_t& abits = a_itr.get_bits();
                 uint64_t& sbits = stage_itr.get_bits();
                 uint64_t& gbits = gb_itr.get_bits();
                 
                 sbits |= gbits; // move all update from gc thread into stage
                 uint64_t delta = sbits & ~gbits; // find out which bits the gc thread has since cleared
             //    assert( (abits & delta) == 0 );

                 _avail += __builtin_popcountll( delta );
                 abits |=  delta; // add those bits to alloc bits
                 //sbits &= ~delta; // remove those bits from stage.
                 sbits -= delta; // remove those bits from stage.
              }
          }

          void* alloc()
          {
              update_avail_list();

              // at this point we have move what we can from the
              // free list to the alloc list... find the first available
              // allocation bit.
              auto b = _alloc_blocks.first_set_bit();
              assert( _alloc_blocks.get(b) );
              //fprintf( stderr, "first set bit _alloc_blocks: %llu   PageSize %lld  / Item Size %lld == %lld\n", b, PageSize, ItemSize, PageSize/ItemSize );
              if( b < PageSize / ItemSize )
              {
                  --_avail;
                  _alloc_blocks.clear(b);
                  assert( !_alloc_blocks.get(b) );
                  //fprintf( stderr, "allocate %p,  index %d\n", _data + b*ItemSize, b );
          //        fprintf( stderr, "after clear.. first set bit _alloc_blocks: %llu\n", 
           //                                                 _alloc_blocks.first_set_bit() );
                  return _data + b*ItemSize;
              }
 //             fprintf( stderr, "first available bit...: %llu for PageSize: %llu  ItemSize: %llu PageNum: %llu\n", b, PageSize, ItemSize, (int64_t(_data)&0xffffffff)/PageSize );
              return nullptr;
          }

          int64_t available()const
          {
              return _avail;
          }

          bool free( void* c )
          {
              int64_t offset = ((char*)c) - _data;
              if( offset >= 0 && offset < PageSize )
              {
                 _alloc_blocks.set( offset / ItemSize );
                 ++_avail;
                 return true;
              }
          //    fprintf( stderr, "ERROR\n" );
              return false;
          }

          bool is_claimed()const
          {
            return 0 != _claim.load(std::memory_order_relaxed);
          }

          bool claim()
          {
            return 0 == _claim.fetch_add(1);
          }

          void release() 
          {
            _claim.store(0);
          }
       protected:
          friend class thread_local_heap;
          friend class fixed_pool;

          int64_t             _avail; // count managed by alloc thread

          std::atomic<int>    _claim; // when 0 no one owns this page, first person to inc owns the page.
          
          /** The GC thread sets bits here when it receives a free block*/
          //bit_index<PageSize/ItemSize> _free_blocks;  // updated by gc thread
          bit_index<64*64*64> _free_blocks;  // updated by gc thread

          /** The alloc thread copies bits from _free_blocks and stages them here
           *  until the gc_thread clears the bits in _free_blocks.  This prevents
           *  two threads from having to compete to update the same memory and
           *  eliminates the need for atomic operations.  Eventually the cache 
           *  will sync up.
           */
          bit_index<64*64*64> _stage_blocks; // updated by active thread

          /**
           *  The thread that 'owns' this page moves free bits from _stage_blocks
           *  to alloc blocks once they are no longer in _free_blocks.  It then
           *  uses free bits to satisify calls to alloc.
           */
          bit_index<64*64*64> _alloc_blocks; // updated by active thread
          

          char*                        _data;
    }; // class page


    /**
     *  Grab the next page with free space or allocate on
     *  if necessary.  This method may be called from any
     *  thread.
     */
    virtual basic_page* claim_page()
    {
        auto first_free = _free_pages.first_set_bit();
        do
        {
            //fprintf( stderr, "claim page first free %llu\n", first_free );
            if( _pages[first_free] == nullptr )
            {
                int64_t claim = _next_page.fetch_add(1);
                //fprintf( stderr, "       ALLOC  claim page first free %lld\n", claim );
                page* p = new page(claim);
                // TODO: set the page location.. 
                // Page Location = 2*GB * ItemSize + PageSize * claim
                
                // Given a random pointer we can find its 'size' by
                // shifting the pointer >> 32   We can find the 'page'
                // by subtracting the base address (size) from the
                // pointer address and dividing by the PageSize, we
                // can then find the 'item' or 'bit' within the page
                // by % ItemSize, once we know these things then it 
                // is a simple matter for the gc thread to do the
                // following to 'free' the item:
                //  
                //  fixed_pool<>::get().
                //  fixed_pools[size]->free(page,item)//_pages[page].free(item);

                p->claim();
                _pages[claim] = p;
                return p;
            }
            if( _pages[first_free]->claim() )
              return _pages[first_free];

            // TODO: replace with iterator next_free..
            first_free++; // = _free_pages.first_set_bit();
        } while( true ); 
    }

    virtual bool gc_free( void* v )
    {
        int64_t byte_pos      = (int64_t(v)<<32)>>32;
        int64_t page_num      = byte_pos/(PageSize);
        int64_t page_byte_pos = byte_pos%(PageSize);
        auto pg = _pages[page_num];
        int64_t item_num = page_byte_pos / ItemSize;
        //fprintf( stderr, "%p  page: %lld  item %lld\n", v, page_num, item_num );
        assert( pg );
        if( pg  )
        {
          //assert( !pg->_free_blocks.get(item_num) );
          pg->_free_blocks.set(item_num);
          auto free_itr  = pg->_free_blocks.at( item_num );
          free_itr.set();
          //auto stage_itr = pg->_stage_blocks.at( item_num );
          uint64_t& fbits = free_itr.get_bits();
          fbits -= pg->_stage_blocks.get_bits(item_num) & fbits;

          if( pg->available() == 0 && !pg->is_claimed() )
          {
              assert( _free_pages.get( page_num ) == false );
              pg->update_avail_list(); 
              _free_pages.set( page_num );
          }
          return true;
        }
        return false;
    }
    fixed_pool()
    {
       _free_pages.set_all();
       memset( _pages, 0, sizeof(_pages) );
    }

    typedef page*        page_ptr;
    std::atomic<int>     _next_page; // inc to allocate a new page.

    // updated by gc thread... 'unclaimed pages' with free data.
    bit_index<64*64/*2*GB/PageSize*/>  _free_pages;
    page_ptr                  _pages[2*GB/PageSize];
};

struct free_node
{
  free_node* next;
};
class thread_local_heap;

class garbage_collector
{
  public:
    garbage_collector()
    :_done(false),
      _tlheaps(nullptr),
     _gc_thread(&garbage_collector::run){}
    ~garbage_collector()
    {
      _done.store(true);
      _gc_thread.join();
    }

    void register_thread_local_heap( thread_local_heap* t );

    static garbage_collector& get()
    {
      static garbage_collector gc;
      return gc;
    }

    static void run();

  private:
    std::atomic<bool>               _done;
    std::atomic<thread_local_heap*> _tlheaps;
    std::thread                     _gc_thread;
};

static basic_pool_ptr get_pool( int p )
{
  assert( p >= 0 && p < 16 );
  static basic_pool_ptr _pools[16];
  static bool           _init = [&]()->bool{
     // allocate the pools for all size classes
     _pools[0]  = new fixed_pool<16>();
     _pools[1]  = new fixed_pool<32>();
     _pools[2]  = new fixed_pool<64>();
     _pools[3]  = new fixed_pool<128>();
     _pools[4]  = new fixed_pool<256>();
     _pools[5]  = new fixed_pool<512>();
     _pools[6]  = new fixed_pool<1024>();
     _pools[7]  = new fixed_pool<2*1024>();
     _pools[8]  = new fixed_pool<4*1024>();
     _pools[9]  = new fixed_pool<8*1024>();
     _pools[10] = new fixed_pool<16*1024>();
     _pools[11] = new fixed_pool<32*1024>();
     _pools[12] = new fixed_pool<64*1024>();
     _pools[13] = new fixed_pool<128*1024>();
     _pools[14] = new fixed_pool<256*1024>();
     _pools[15] = new fixed_pool<512*1024>();
     return true;
  }();
  (void)_init; // unused warning
  return _pools[p];
}


class thread_local_heap
{
   public:
      thread_local_heap()
      :_gc_at_bat(nullptr),
       _gc_on_deck(nullptr)
      {
        garbage_collector::get().register_thread_local_heap(this);
      }

      ~thread_local_heap()
      {
      }

      static thread_local_heap& get()
      {
        static __thread thread_local_heap* tlh = nullptr;
        if( !tlh ) tlh = new thread_local_heap();
        return *tlh;
      }

      void* alloc( size_t s )
      {
          int32_t pool  = LOG2(s-1) + 1 - 4;
      //    fprintf( stderr, "pool %d  for size %u", pool, s );

          if( !_pages[pool] )
          {
              basic_page_ptr p = get_pool(pool)->claim_page();
              _pages[pool] = p;
              auto r = p->alloc();
              assert(r);
              return r;
          }
          void* a = _pages[pool]->alloc();

          if( !a )  // the page must be full... release it and get a new one
          {
              _pages[pool]->release();
              basic_page_ptr p = get_pool(pool)->claim_page();
              _pages[pool] = p;
              assert(p);
              auto r = p->alloc();
              assert(r);
              return r; p->alloc();
          }
 //         fprintf( stderr, "alloc %p        tld: %p\n", a, this );
          assert( a );
          return a;
      }

      void  free( void* v )
      {
          assert( v != nullptr );

     //     fprintf( stderr, "free %p      tld: %p\n", v, this );
          free_node* fv = (free_node*)v;
          assert( fv != _gc_on_deck );
          
          size_t   s     = int64_t(v)>>32;
          int32_t  pool  = LOG2(s) - 4;

  //        fprintf( stderr, "Free size: %llu  on pool %d\n", s, pool );

          // try local free first.
           if( _pages[pool] && _pages[pool]->free(v) )
              return;

          fv->next = _gc_on_deck;
          _gc_on_deck = fv;

          if( _gc_at_bat == nullptr )
          {
            _gc_at_bat = _gc_on_deck;
            _gc_on_deck = nullptr;
          }
      }

   private:
      friend class garbage_collector;

      free_node*         _gc_at_bat;
      free_node*         _gc_on_deck;
                         
      basic_page_ptr     _pages[32]; // sized every power of 2 up to 1MB
      thread_local_heap* _next;
};


void garbage_collector::register_thread_local_heap( thread_local_heap* t )
{
   auto* stale_head = _tlheaps.load(std::memory_order_relaxed);
   do { t->_next = stale_head;
   }while( !_tlheaps.compare_exchange_weak( stale_head, t, std::memory_order_release ) );
}

void garbage_collector::run()
{
  garbage_collector& gc = garbage_collector::get();
  while( true )
  {
    bool found_work = false;
    thread_local_heap* cur = gc._tlheaps.load( std::memory_order_relaxed );
    while( cur )
    {
        free_node* n = cur->_gc_at_bat;
        if( n )
        {
          cur->_gc_at_bat = nullptr;
          found_work = true;
        }
        while( n )
        {
          auto next = n->next;
          // TODO: free N
          int pool = LOG2( int64_t(n) >> 32 ) - 4;
          get_pool( pool )->gc_free(n);
          //fprintf( stderr, "." );
          assert( n != next );
          n = next;
        }
        assert( cur != cur->_next );
        cur = cur->_next;
    }
    if( !found_work )
    {
       // TODO: replace with something better..
       ::usleep( 10 );
       if( gc._done.load() ) return;
    }
  }
}



void* fp_malloc( size_t s )
{
  return thread_local_heap::get().alloc(s);
}

void fp_free( void* v )
{
  thread_local_heap::get().free(v);
}
