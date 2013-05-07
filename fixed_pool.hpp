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
     basic_page():_next_page(nullptr){}
    virtual ~basic_page(){}
    virtual void  release() = 0;
    virtual void* alloc() = 0;
    virtual void  free( void* ) = 0;
    virtual int   get_page_pos() = 0;
    virtual int   get_pool() = 0;
    virtual int64_t   get_available()const = 0;
    basic_page* _next_page;
 //   virtual void  item_size()const = 0;
};

typedef basic_page* basic_page_ptr;
class basic_pool
{
  public:
    virtual ~basic_pool(){}
    virtual basic_page* claim_page() = 0;
    virtual bool  gc_free(void*) = 0;
    virtual void gc_release( basic_page_ptr p ) = 0;
};
typedef basic_pool* basic_pool_ptr;


struct free_node
{
  free_node* next;
};

template<uint64_t ItemSize, uint64_t PageSize = 1*MB>
class fixed_pool : public basic_pool
{
  public:
    
    class page : public basic_page
    {
       public:
          page( int64_t claim_pos )
          {
              fprintf( stderr, "CLAIM POS %lld\n", claim_pos );
              _data = (char*)mmap_alloc( PageSize, (void*)((ItemSize << 32) + claim_pos * PageSize) );
              fprintf( stderr, " PAGE DATA: %p\n", _data );
              assert( (int64_t(_data) >> 32) == ItemSize );
              _next_data       = _data;
              _page_end        = _data + PageSize;
              _alloc_free      = nullptr;
              _gc_free_at_bat  = nullptr;
              _gc_free_on_deck = nullptr;
              _claim_pos = claim_pos;
              _alloc = 0;
              _free  = 0;
          }

          int _claim_pos;
          virtual int   get_page_pos() { return _claim_pos; }

          int get_pool() { return LOG2(ItemSize)-4; }

          ~page()
          {
            mmap_free( _data, PageSize );
          }

          void* alloc()
          {
              if( _gc_free_at_bat )
              {
                  fprintf( stderr, "%p   _gc_free_at_bat   page pos %d\n", this, _claim_pos );
                 free_node* gc = _gc_free_at_bat;
                 _gc_free_at_bat = nullptr;

                 while( gc )
                 {
                    free_node* n = gc->next;
                    gc->next = _alloc_free;
                    _alloc_free = gc;
                    gc = n;
                 }
              }
              if( _alloc_free )
              {
                 free_node* n = _alloc_free;
                 _alloc_free = n->next;
                 ++_alloc;
                 return n;
              }
              else if( _next_data != _page_end )
              {
                char* n = _next_data;
                _next_data += ItemSize;
                assert( n < _page_end );
                ++_alloc;
                return n;
              }
              else
              {
                fprintf( stderr, "_next_data == _page_end\n" );
                return nullptr;
              }
          }

          int64_t get_available()const
          {
              return PageSize/ItemSize - _alloc + _free; //_avail;
          }

          void free( void* c )
          {
              assert( c > _data && c < _page_end );
              free_node* n = (free_node*)c;
              n->next = _alloc_free;
              _alloc_free = n;
          }

          void gc_free( void* c )
          {
              //fprintf( stderr, "gc_free(%p)   _data %p   _end %p\n", c, _data, _page_end );
              assert( c >= _data && c < _page_end );
              free_node* n = (free_node*)c;
              n->next = _gc_free_on_deck;
              _gc_free_on_deck = n;

              if( !_gc_free_at_bat )
              {
                _gc_free_at_bat = _gc_free_on_deck;
                _gc_free_on_deck = nullptr;
              }
              ++_free;
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

          int64_t             _alloc; // count managed by alloc thread
          int64_t             _free;  // count managed by the gc thread

          std::atomic<int>    _claim; // when 0 no one owns this page, first person to inc owns the page.
          
          free_node*          _alloc_free; // free list managed by alloc thread
                              
          free_node*          _gc_free_at_bat; 
          free_node*          _gc_free_on_deck;
          char*               _data;
          char*               _page_end;
          char*               _next_data;

    }; // class page


    /**
     *  Grab the next page with free space or allocate on
     *  if necessary.  This method may be called from any
     *  thread.
     */
    virtual basic_page* claim_page()
    {
        auto rp = _pending_read_pos.load( std::memory_order_relaxed );
        auto wp = _pending_write_pos.load( std::memory_order_relaxed );
        if( rp <= wp )
        {
          int64_t claim = _pending_read_pos.fetch_add(1);
          if( claim <= wp )
          {
             basic_page* p = _pending_pages[claim%32];
             _pending_pages[claim%32] = 0;
             if( p )
             {
              fprintf( stderr, "claiming pending page %p  \n", p);//, p->get_page_pos() );
              return p;
             }
             else
             {
              fprintf( stderr, "pending pages[claim] == null\n" );
             }
          }
        }
        
        int64_t claim = _next_page.fetch_add(1);
        page* p = new page(claim);
        fprintf( stderr, "alloc new page pending page %p  %d\n", p, p->get_page_pos() );
        //p->claim();
        _pages[claim] = p;
        return p;
    }

    virtual bool gc_free( void* v )
    {
        int64_t byte_pos      = (int64_t(v)<<32)>>32;
        int64_t page_num      = byte_pos/(PageSize);
        auto pg = _pages[page_num];
        fprintf( stderr, "page_num %lld  %p\n", page_num, v );
        assert( pg );
        if( pg  )
        {
          pg->gc_free(v);
          return true;
        }
        return false;
    }
    virtual void gc_release( basic_page_ptr p )
    {
       _free_pages.set( p->get_page_pos() );
       auto rp = _pending_read_pos.load(std::memory_order_relaxed);
       auto wp = _pending_write_pos.load(std::memory_order_relaxed);
       while( rp > wp - 31 )
       {
          ++wp;
          auto pos = wp%32;
          if( _pending_pages[pos] == nullptr )
          {
            int b = _free_pages.first_set_bit();
            if( _pages[b] && _pages[b]->get_available() )
            {
              _free_pages.clear(b);
              fprintf( stderr, "pending_pages[%lld] = %p\n", pos, _pages[b] );
              _pending_pages[ pos ] = _pages[b];
            }
            if( !_pages[b] ){ --wp; break; }
         }
       }
       _pending_write_pos.store(wp);
    }

    fixed_pool()
    :_pending_read_pos(0),_pending_write_pos(-1)
    {
       _free_pages.set_all();
       memset( _pages, 0, sizeof(_pages) );
       memset( _pending_pages, 0, sizeof(_pending_pages) );
    }

    typedef page*        page_ptr;
    std::atomic<int>     _next_page; // inc to allocate a new page.

    std::atomic<int64_t> _pending_read_pos;
    std::atomic<int64_t> _pending_write_pos;
    page_ptr             _pending_pages[32];

    // updated by gc thread... 'unclaimed pages' with free data.
    bit_index<64*64/*2*GB/PageSize*/>  _free_pages;
    page_ptr                  _pages[2*GB/PageSize];
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
  if( !(p >= 0 && p < 16 ) )
      fprintf( stderr, "%d", p );
  assert( (p >= 0 && p < 16 ) );
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
       _release_at_bat(nullptr),
       _gc_on_deck(nullptr),
       _release_on_deck(nullptr)
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
   //       fprintf( stderr, "pool %d  for size %d\n", pool, int(s) );

          if( !_pages[pool] )
          {
              basic_page_ptr p = get_pool(pool)->claim_page();
              fprintf( stderr, "claim pool! %p\n", p );
              assert(p);
              _pages[pool] = p;
              auto r = p->alloc();
              assert(r);
              return r;
          }
          void* a = _pages[pool]->alloc();

          if( !a )  // the page must be full... release it and get a new one
          {
              fprintf( stderr, "release pool %d  %p\n", pool, _pages[pool] );
              basic_page_ptr p = get_pool(pool)->claim_page();
              assert( p );
              fprintf( stderr, "new page %p   avail: %lld\n", p, p->get_available() );

              _pages[pool]->_next_page = _release_on_deck;
              _release_on_deck = _pages[pool];

              if( _release_at_bat == nullptr )
              {
                _release_at_bat = _release_on_deck;
                _release_on_deck = nullptr;
              }
              _pages[pool] = p;
              assert(p);
              auto r = p->alloc();
              assert(r);
              return r;
          }
          assert( a );
          return a;
      }

      void  free( void* v )
      {
          assert( v != nullptr );

     //     fprintf( stderr, "free %p      tld: %p\n", v, this );
          
         // size_t   s     = int64_t(v)>>32;
         // int32_t  pool  = LOG2(s) - 4;

  //        fprintf( stderr, "Free size: %llu  on pool %d\n", s, pool );

          // try local free first.
         //  if( _pages[pool] && _pages[pool]->free(v) )
         //     return;

          free_node* fv = (free_node*)v;
          assert( fv != _gc_on_deck );

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
      basic_page_ptr     _release_at_bat;
      uint64_t           _gc_pad[7];
      free_node*         _gc_on_deck;
      basic_page_ptr     _release_on_deck;
                         
      // current page for this thread...
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
       //   fprintf( stderr, "pool %d  gc_free %p\n", pool, n );
          get_pool( pool )->gc_free(n);
          //fprintf( stderr, "." );
          assert( n != next );
          n = next;
        }
        if( cur->_release_at_bat != nullptr )
        {
           basic_page_ptr p = cur->_release_at_bat;
           cur->_release_at_bat = nullptr;

           while( p )
           {
              p->release();
              int pool = p->get_pool(); //LOG2( int64_t(p) >> 32 ) - 4;
              get_pool( pool )->gc_release(p);
              p = p->_next_page;
           }
        }
        assert( cur != cur->_next );
        cur = cur->_next;
    }
    if( !found_work )
    {
       // TODO: replace with something better..
       ::usleep( 100 );
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
