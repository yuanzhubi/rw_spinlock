#ifndef RW_SPINLOCK_H_
#define RW_SPINLOCK_H_
#include <inttypes.h>

struct rw_spinlock_type{
    //WRITER_FIRST means even the writer lock failed, it also blocks other readers lock.
    //WRITER_PRIOR is almost the same with WRITER_FIRST but it will check whether readers have locked before its lock.
    //FAIR is completely fair in group lock,
    typedef unsigned char rw_spinlock_enum;
    const static rw_spinlock_enum WRITER_FIRST = 0;
    const static rw_spinlock_enum WRITER_PRIOR = 1;
    const static rw_spinlock_enum FAIR = 2;
    const static rw_spinlock_enum READER_PRIOR = 3;
    const static rw_spinlock_enum READER_FIRST = 4;
};

template <rw_spinlock_type::rw_spinlock_enum type = rw_spinlock_type::FAIR,
        typename int_type = int32_t, typename int_half_type = int16_t >
struct rw_spinlock{
    const static int_half_type int_half_type_max = (int_half_type)(((uint16_t)-1)>>1);
    const static int_type writer_step = (((int_type)(int_half_type_max)) + 1) << 1;
protected:
    union{
        struct{
            int_half_type reader_count_negative;

            //Positive is locked by writer while negative is locked by readers.
            int_half_type writer_lock;
        }view;

        struct {
            int_type writer_lock;
        }interface;
    }data;

public:
    rw_spinlock(){
        data.interface.writer_lock = 0;
    }
    void reader_lock() volatile {
         while(true){
            while(type < rw_spinlock_type::READER_FIRST && data.interface.writer_lock > 0);
            int_type result = __sync_fetch_and_sub(&data.interface.writer_lock, 1);
            if(result > 0){
                //Writer is quicker than the reader.
                if(type < rw_spinlock_type::READER_PRIOR){
                    //Rollback.
                    __sync_fetch_and_add(&data.interface.writer_lock, 1);
                }else{
                    //Wait for the writer unlock.
                    while(data.view.writer_lock >= 0);
                    break;
                }
            }else{
                break;
            }
        }
    }

    void reader_unlock() volatile {
        __sync_fetch_and_add(&data.interface.writer_lock, 1);
    }

    void writer_lock() volatile {
        while(true){
            if(type > rw_spinlock_type::WRITER_PRIOR){
                //Positive is locked by other writer while negative is locked by readers.
                while(data.view.writer_lock != 0);
                //The CAS operation.
                if(__sync_bool_compare_and_swap(&data.view.writer_lock, 0, int_half_type_max)){
                    break;
                }
            }else{
                //Positive is locked by another writer or with readers.
                if(type == rw_spinlock_type::WRITER_FIRST ){
                    while(data.interface.writer_lock > 0);
                }else{//Positive is locked by other writer while negative is locked by readers.
                    while(data.interface.writer_lock != 0);
                }

                int_type result = __sync_fetch_and_add(&data.interface.writer_lock, writer_step);
                if(result == 0){//You lock first
                    break;
                }else if(result < 0){//Readers lock first, wait them and no new readers come.
                    while(data.view.reader_count_negative != 0);
                    break;
                }else {//Another writer locks first, roll back.
                    __sync_fetch_and_sub(&data.view.writer_lock, 1);
                }
            }
        }
    }

    //Unlock should be wait free, no matter how much threads are competing.
    //But if we do not have large number(usually zero) of threads locking
    // failed in  FAIR mode, busy wait will be quicker than
    // one atomic operation(default behaviour).
    void writer_unlock(bool not_wait_free = false) volatile {
        const static int_half_type step = (type >= rw_spinlock_type::FAIR) ? int_half_type_max : 1;
        if(type <= rw_spinlock_type::FAIR ){
            if(!not_wait_free || type != rw_spinlock_type::FAIR){
                //Version 1: unlock is wait-free
                __sync_fetch_and_sub(&data.view.writer_lock, step);
            }else{
                //Version 2: wait for the readers rollback then unlock
                while(data.view.writer_lock != step);
                data.view.writer_lock = 0;
            }
        }else{
            int_half_type result = data.view.writer_lock;
            if(result == (step - 1)){//The waiting readers will get the lock directly.
                data.view.writer_lock = (int_half_type)(-1);
            }else{
                __sync_fetch_and_sub(&data.view.writer_lock, step);
            }
        }
    }
};

//If all the writers can get lock simultaneously like readers (if and only if
// no readers get lock before), use rw_group_spinlock to speed up.
//The threads in different group can not get the lock simultaneously and other
//cases are allowed. Readers and writers are dual now.
template <rw_spinlock_type::rw_spinlock_enum type = rw_spinlock_type::FAIR,
        typename int_type = int32_t, typename int_half_type = int16_t >
struct rw_group_spinlock :
        protected rw_spinlock<type, int_type, int_half_type> {
private:
    typedef rw_spinlock<type, int_type, int_half_type> super_type;

public:
    void reader_lock_group() volatile {
        super_type::reader_lock();
    }

    void reader_unlock_group() volatile {
        super_type::reader_unlock();
    }


    void writer_lock_group() volatile{
        if(type <= rw_spinlock_type::WRITER_PRIOR){
            while(type == rw_spinlock_type::WRITER_PRIOR && super_type::data.view.reader_count_negative != 0);
            __sync_fetch_and_add(&super_type::data.view.writer_lock, 1);
            //Wait the readers unlock or rollback.
            while(super_type::data.view.reader_count_negative != 0);
        }else{
            while(true){
                while (super_type::data.view.reader_count_negative != 0 );
                if((__sync_fetch_and_add(&super_type::data.interface.writer_lock, super_type::writer_step)
                   & (super_type::writer_step - 1)) == 0){
                    break;
                }else{
                    //May be a writer succeed first but we do not wait the readers rollback for fair.
                    __sync_fetch_and_sub(&super_type::data.view.writer_lock, 1);
                }
            }

        }
    }

    void writer_unlock_group() volatile{
        __sync_fetch_and_sub(&super_type::data.view.writer_lock, 1);
    }
};

#ifdef  TEST
int main(){
    rw_spinlock<> a;
    rw_group_spinlock<> b;
    return 0;
}
#endif

#endif
