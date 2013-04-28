

/*
pool<24>   p24;
pool<58>   p58;
pool<120>  p120;
pool<248>  p248;
pool<504>  p504;
pool<1016> p1016;
pool<2040> p2040;
pool<4088> p4088;
*/


void* fc_malloc( size_t s )
{
#define TRY_POOL(I,X,S)   if( len < X ) return pool<I,X,S>::alloc(); 
    TRY_POOL(1,24,256);
    TRY_POOL(2,58,256);
    TRY_POOL(3,120,256);
    TRY_POOL(4,248,128);
    TRY_POOL(5,504,128);
    TRY_POOL(6,1016,128);
    TRY_POOL(7,2040,64);
    TRY_POOL(8,4088,64);
    TRY_POOL(9,8184,64);



    if( len < 64*1024 )
    {
    }
    if( len < 1024*1024 )
    {

    }
    else
    {
       uint64_t* m = malloc( s+8);
       *m = -1;
       return m+1;
    }
}

free( void* f )
{

}
