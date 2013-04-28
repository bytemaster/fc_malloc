#pragma once
#include <memory>
#include <vector>
#include <stdint.h>
#include <unistd.h>
#include <atomic>
#include <assert.h>
#include <iostream>

namespace disruptor
{

class eof : public std::exception
{
   public:
    virtual const char* what()const noexcept { return "eof"; }
};


/**
 *  A sequence number must be padded to prevent false sharing and
 *  access to the sequence number must be protected by memory barriers.
 *
 *  In addition to tracking the sequence number, additional state associated
 *  with the sequence number is also made available.  No false sharing 
 *  should occur because all 'state' is only written by one thread. This
 *  extra state includes whether or not this sequence number is 'EOF' and
 *  whether or not any alerts have been published.
 */
class sequence
{
   public:
      sequence( int64_t v = 0 ):_sequence(v),_alert(0){}

      int64_t  lazy_read()const                   { return *((volatile int64_t*)&_sequence);}// .load( std::memory_order_acquire); }
      //volatile int64_t& lazy_write()              { return *((volatile int64_t*)&_sequence);}// .load( std::memory_order_acquire); }
      int64_t  aquire()const                      { return _sequence.load( std::memory_order_acquire); }
      int64_t  aquire_pending()const              { return _pending_sequence.load( std::memory_order_acquire); }
      void     lazy_store( int64_t value )        { _sequence.store(value, std::memory_order_relaxed); }
      void     store( int64_t value )             { _sequence.store(value, std::memory_order_release); }
      void     store_pending( int64_t value )     { _pending_sequence.store(value, std::memory_order_release); }
      void     set_eof()    { _alert = 1; }
      void     set_alert()  { _alert = -1; }
      bool     eof()const   { return _alert == 1; }
      bool     alert()const { return _alert != 0; }

      int64_t atomic_increment_and_get( uint64_t inc ) 
      { 
        return _sequence.fetch_add(inc, std::memory_order::memory_order_release) + inc;
      }

      int64_t increment_and_get( uint64_t inc ) 
      { 
          auto tmp = aquire() + inc;
          store( tmp );
          return tmp;
      }

   private:
      std::atomic<int64_t> _sequence;
      volatile int64_t     _alert;
      std::atomic<int64_t> _pending_sequence;
      int64_t              _post_pad[5];
};

class event_cursor;

/**
 *   A barrier will block until all cursors it is following are
 *   have moved past a given position.  The barrier uses a
 *   progressive backoff strategy of busy waiting for 1000 
 *   tries, yielding for 1000 tries, and the usleeping in 10 ms
 *   intervals.   
 *
 *   No wait conditions or locks are used because they would
 *   be 'intrusive' to publishers which must check to see whether
 *   or not they must 'notify'.  The progressive backoff approach
 *   uses little CPU and is a good compromise for most use cases.
 */
class barrier 
{
   public:
      void follows( const event_cursor& e );

      /**
       *  Used to check how much you can read/write without blocking.
       *
       *  @return the min position of every cusror this barrier follows.
       */
      int64_t get_min();

      /*
       *  This method will wait until all s in seq >= pos using a progressive
       *  backoff of busy wait, yield, and usleep(10*1000)
       *
       *  @return the minimum value of every dependency
       */
      int64_t wait_for( int64_t pos )const;
   private:
      mutable int64_t                   _last_min;
      std::vector<const event_cursor*>  _limit_seq;
};

/**
 *  Provides a automatic index into a ringbuffer with
 *  a power of 2 size.
 */
template<typename EventType, uint64_t Size = 1024>
class ring_buffer
{
    public:
      typedef EventType event_type;

      static_assert( ((Size != 0) && ((Size & (~Size + 1)) == Size)), 
                     "Ring buffer's must be a power of 2" );

      /** @return a read-only reference to the event at pos */
      const EventType& at( int64_t pos )const 
      {
        return _buffer[pos & (Size-1)];
      }

      /** @return a reference to the event at pos */
      EventType& at( int64_t pos )
      {
        return _buffer[pos & (Size-1)];
      }

      /** useful to check for contiguous ranges when EventType is
       *  POD and memcpy can be used.  OR if the buffer is being used
       *  by a socket dumping raw bytes in.  In which case memcpy
       *  would have to use to ranges instead of 1.
       */
      int64_t get_buffer_index( int64_t pos )const { return pos & (Size-1); }
      int64_t get_buffer_size()const               { return Size;           }

    private:
      EventType            _buffer[Size];
};

/**
 *  A cursor is used to track the location of a publisher / subscriber within
 *  the ring buffer.  Cursors track a range of entries that are waiting
 *  to be processed.  After a cursor is 'done' with an entry it can publish
 *  that fact.  
 *
 *  There are two types of cursors, read_cursors and write cursors.  read_cursors
 *  block when they need to 
 *
 *  Events between [begin,end) may be processed at will for readers.  When a reader
 *  is done they can 'publish' their progress which will move begin up to
 *  published position+1.   When begin == end, the cursor must call wait_for(end), 
 *  wait_for() will return a new 'end'.  
 *
 *  @section read_cursor_example Read Cursor Example
 *  @code
      auto source   = std::make_shared<ring_buffer<EventType,SIZE>>();
      auto dest     = std::make_shared<ring_buffer<EventType,SIZE>>();
      auto p        = std::make_shared<write_cursor>("write",SIZE);
      auto a        = std::make_shared<read_cursor>("a");

      a->follows(p);
      p->follows(a);

      auto pos      = a->begin();
      auto end      = a->end();
      while( true ) 
      {
         if( pos == end )
         {
             a->publish(pos-1);
             end = a->wait_for(end);
         }
         dest->at(pos) = source->at(pos);
         ++pos;
      }
 *  @endcode
 *
 *
 *  @section write_cursor_example Write Cursor Example
 *
 *  The following code would run in the publisher thread.  The
 *  publisher can write data without 'waiting' until it pos is
 *  greater than or equal to end.  The 'initial condition' of
 *  a publisher is with pos > end because the write cursor
 *  cannot 'be valid' for readers until after the first element
 *  is written.  
 *
    @code
        auto pos = p->begin();
        auto end = p->end();
        while( !done )
        {
           if( pos >= end )
           {  
              end = p->wait_for(end);
           }
           source->at( pos ) = i;
           p->publish(pos);
           ++pos;
        }
        // set eof to signal any followers to stop waiting after
        // they hit this position.
        p->set_eof();
    @endcode
 *
 *
 *
 */
class event_cursor
{
   public:
      event_cursor(int64_t b=-1):_name(""),_begin(b),_end(b){}
      event_cursor(const char* n, int64_t b=0):_name(n),_begin(b),_end(b){}

      /** this event processor will process every event
       *  upto, but not including s
       */
      void follows( const event_cursor& s ) { _barrier.follows(s); }

      /** returns one after cursor */
      int64_t begin()const { return _begin; }

      /** returns one after the last ready as of last call to wait_for() */
      int64_t end()const   { return _end;   }


      /** makes the event at p available to those following this cursor */
      void     publish( int64_t p )
      {
         check_alert();
         _begin = p + 1;
         _cursor.store( p );
      }
      void    lazy_publish( int64_t p )
      {
         _begin = p + 1;
         _cursor.lazy_store(p);
      }

      /** when the cusor hits the end of a stream, it can set the eof flag */
      void set_eof(){ _cursor.set_eof(); }

      /** If an error occurs while processing data the cursor can set an 
       *  alert that will be thrown whenever another cursor attempts to wait
       *  on this cursor.
       */
      void  set_alert( std::exception_ptr e ) 
      {   
          _alert = std::move(e); 
          _cursor.set_alert(); 
      }

      /** @return any alert set on this cursor */
      const std::exception_ptr& alert()const { return _alert; }


      /** If an alert has been set, throw! */
      inline void check_alert()const; 

      /** the last sequence number this processor has 
       *  completed.
       */
      const sequence& pos()const { return _cursor; }
      sequence&       pos(){ return _cursor; }

      /** used for debug messages */
      const char* name()const { return _name; }

    protected:
      /** last know available, min(_limit_seq) */
      const char*                   _name;
      int64_t                       _begin;
      int64_t                       _end;
      std::exception_ptr            _alert;
      barrier                       _barrier;
      sequence                      _cursor;
};

/**
 *  Tracks the read position in a buffer
 */
class read_cursor : public event_cursor
{
    public:
      read_cursor(int64_t p=0):event_cursor(p){}
      read_cursor(const char* n, int64_t p=0):event_cursor(n,p){}

      /** @return end() which is > pos */
      int64_t wait_for( int64_t pos )
      {
         try {
          return _end = _barrier.wait_for(pos) + 1;
         }
         catch ( const eof& ) { _cursor.set_eof(); throw; }
         catch ( ... ) { set_alert( std::current_exception() ); throw; }
      }

      /** find the current end without blocking */
      int64_t check_end()
      {
          return _end = _barrier.get_min() + 1;
      }
};

class shared_read_cursor : public read_cursor
{
    public:
      shared_read_cursor(int64_t p=0):read_cursor(p){}
      shared_read_cursor(const char* n, int64_t p=0):read_cursor(n,p){}

      /**
       *  This method will block until 'after_pos' is the 
       *  current pos, then it will set pos to 'pos'
       */
      void publish_after( int64_t pos, int64_t after_pos )
      {
         try {
            assert( pos > after_pos );
            while( _cursor.aquire() < after_pos )
            {
              // TODO:... this is a spinlock, ease CPU HERE... 
            } 
            // _barrier.wait_for(after_pos);
            publish( pos );
         }
         catch ( const eof& ) { _cursor.set_eof(); throw; }
         catch ( ... ) { set_alert( std::current_exception() ); throw; }
      }

      bool is_available( int64_t pos )
      {
         return pos <= _barrier.get_min(); 
      }

      int64_t claim(int64_t num) 
      {  
         auto pos = _claim_cursor.atomic_increment_and_get( num );
         return pos - num;
      }


      sequence      _claim_cursor;
};

typedef std::shared_ptr<read_cursor> read_cursor_ptr;

/**
 *  Tracks the write position in a buffer.
 *
 *  Write cursors need to know the size of the buffer
 *  in order to know how much space is available. 
 */
class write_cursor : public event_cursor
{
    public:
      /** @param s - the size of the ringbuffer, 
       *  required to do proper wrap detection 
       **/
      write_cursor(int64_t s)
      :_size(s),_size_m1(s-1)
      {
        _begin = 0;
        _end   = _size;
        _cursor.store(-1);
      }

      /**
       * @param n - name of the cursor for debug purposes
       * @param s - the size of the buffer.  
       */
      write_cursor(const char* n, int64_t s)
      :event_cursor(n),_size(s),_size_m1(s-1)
      {
         _begin = 0;
         _end   = _size;
         _cursor.store(-1);
      }

      /** waits for begin() to be valid and then
       *  returns it.  This is only safe for 
       *  single producers, multi-producers should 
       *  use claim(1) instead.
       */
      int64_t wait_next() 
      {
          wait_for( _begin );
          return _begin;
      }

      /**
       *   We need to wait until the available space in
       *   the ring buffer is  pos - cursor which means that
       *   all readers must be at least to pos - _size and
       *   that our new end is the min of the readers + _size
       */
      int64_t wait_for( int64_t pos )
      {
         try 
         {
           // throws exception on error, returns 'short' on eof
           return _end = _barrier.wait_for(  pos - _size ) + _size;  
         } 
         catch ( ... ) 
         { 
            set_alert( std::current_exception() ); throw; 
         }
      }
      int64_t check_end()
      {
          return _end = _barrier.get_min() + _size;
      }
    private:
      const int64_t _size;
      const int64_t _size_m1;
};

typedef std::shared_ptr<write_cursor> write_cursor_ptr;
/**
 *  When there are multiple writers this cursor can
 *  be used to reserve space in the write buffer 
 *  in an atomic manner.
 *
 *  @code
 *  auto start = cur->claim(slots);
 *  ... do your writes...
 *  cur->publish_after( start + slots, start -1 );
 *  @endcode
 *
 *  @todo
 *  An alternative implementation of this would involve
 *  having a sequence number for each thread.  A pre-allocated
 *  array of sequence pointers would be initialized to null.
 *  There would be a 'thread-specific' index into this array
 *  that would be allocated by an atomic inc the first time
 *  a new thread attempts to write.   Each sequence number
 *  would maintain two sequence numbers: published and
 *  pending.  
 *
 *  To determine the actual 'position' of the write
 *  cursor one would return the MIN( pending ) -1 or
 *  if no sequences are in the 'pending state' the
 *  MAX(published).  The pending state is any time
 *  the pending > published.
 *
 *  The consequence of this approach is that readers
 *  would have to perform more work to determine the end
 *  (reading from all thread positions), the benefit is
 *  that the producers would never have to 'wait' on
 *  each other.  
 *
 *  A variation on this would be to have a fixed 
 *  set of producers instead of a dynamic set.  This 
 *  fixed set would be configured at the start.
 *
 *  If there is low write-contention then this approach
 *  would probably be poor.
 */
class shared_write_cursor : public write_cursor 
{
   public:
      /** @param s - the size of the ringbuffer, 
       *  required to do proper wrap detection 
       **/
      shared_write_cursor(int64_t s)
      :write_cursor(s){}

      /**
       * @param n - name of the cursor for debug purposes
       * @param s - the size of the buffer.  
       */
      shared_write_cursor(const char* n, int64_t s)
      :write_cursor(n,s){}

      /** When there are multiple writers they cannot both
       *  assume the right to write to begin() to end(), 
       *  instead they must first claim some slots in an
       *  atomic manner.
       *
       *
       *  After pos().aquire() == claim( slots ) -1 the claimer
       *  is free to call publish up to start + slots -1 
       *
       *  @return the first slot the caller may write to.
       */   
      int64_t claim( size_t num_slots )
      {
           auto pos = _claim_cursor.atomic_increment_and_get( num_slots );
      //     std::cerr<<"  shared_write: publish "<<pos<<" after " << (pos-1) << " current pos: "<<_cursor.aquire()<<"\n";
           // make sure there is enough space to write
           wait_for( pos -1 ); // TODO: -1????
           return pos - num_slots;
      }

      /**
       *  This method will block until 'after_pos' is the 
       *  current pos, then it will set pos to 'pos'
       */
      void publish_after( int64_t pos, int64_t after_pos )
      {
         try {
            assert( pos > after_pos );
          //  std::cerr<<"publish "<<pos<<" after " << after_pos << " current pos: "<<_cursor.aquire()<<"\n";
            while( _cursor.aquire() != after_pos )
            {
              // TODO:... this is a spinlock, ease CPU HERE... 
              usleep(0);
            } 
            // _barrier.wait_for(after_pos);
            publish( pos );
         }
         catch ( const eof& ) { _cursor.set_eof(); throw; }
         catch ( ... ) { set_alert( std::current_exception() ); throw; }
      }
    private:
      sequence      _claim_cursor;
};



typedef std::shared_ptr<shared_write_cursor> shared_write_cursor_ptr;



inline void barrier::follows( const event_cursor& e )
{
    _limit_seq.push_back( &e );
}

inline int64_t barrier::get_min()
{
   int64_t min_pos = 0x7fffffffffffffff;
   for( auto itr = _limit_seq.begin(); itr != _limit_seq.end(); ++itr )
   {
      auto itr_pos = (*itr)->pos().aquire();
      if( itr_pos < min_pos ) min_pos = itr_pos;
   }
   return _last_min = min_pos;
}

inline int64_t barrier::wait_for( int64_t pos )const
{
   if( _last_min > pos ) 
      return _last_min;

   int64_t min_pos = 0x7fffffffffffffff;
   for( auto itr = _limit_seq.begin(); itr != _limit_seq.end(); ++itr )
   {
      int64_t itr_pos = 0;
      itr_pos = (*itr)->pos().aquire();
      // spin for a bit 
      for( int i = 0; itr_pos < pos && i < 10000; ++i  )
      {
         itr_pos = (*itr)->pos().aquire();
         if( (*itr)->pos().alert() ) break;
      }
      // yield for a while, queue slowing down
      for( int y = 0; itr_pos < pos && y < 10000; ++y )
      {
         usleep(0);
         itr_pos = (*itr)->pos().aquire();
         if( (*itr)->pos().alert() ) break;
      }

      // queue stalled, don't peg the CPU but don't wait
      // too long either...
      while( itr_pos < pos )
      {
         usleep( 10*1000 );
         itr_pos = (*itr)->pos().aquire();
         if( (*itr)->pos().alert() ) break;
      }

      if( (*itr)->pos().alert() )
      {
         (*itr)->check_alert();
         if( itr_pos > pos ) 
            return itr_pos -1; // process everything up to itr_pos
         throw eof();
      }


      if( itr_pos < min_pos ) 
          min_pos = itr_pos; 
   }
   //assert( min_pos != 0x7fffffffffffffff );
   return _last_min = min_pos;
}

inline void event_cursor::check_alert()const
{
    if( _alert != std::exception_ptr() ) std::rethrow_exception( _alert );
}


} // namespace disruptor
