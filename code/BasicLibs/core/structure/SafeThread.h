#ifndef _MGEN_SAFE_THREAD_H_
#define _MGEN_SAFE_THREAD_H_

#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <utility>

namespace MGEN
{
    using ThreadFuncImpl = std::function<void(void)>;

    class SafeThread
    {
    public:
        // constructor
        SafeThread() = default;

        // destructor
        ~SafeThread()
        {
            this->Stop();
        }

        void SetThreadFunctions( ThreadFuncImpl runner, ThreadFuncImpl closer = nullptr ) noexcept
        {
            if( runner ) this->thread_runner_ = std::move(runner);
            if( closer ) this->thread_closer_ = std::move(closer);
        }

        bool Start() noexcept
        {
            if( this->is_running_.exchange(true) == false ){
                this->thread_object_ = std::thread([this](){
                    if( this->thread_runner_ )
                        this->thread_runner_();
                } );
            }
            return true;
        }

        void Stop()
        {
            if( this->is_running_.exchange(false) == true ) {
                if( this->thread_closer_ )
                    this->thread_closer_();

                if( this->thread_object_.joinable() )
                    this->thread_object_.join();
            }
        }

        std::atomic<bool>& GetRunningFlag() noexcept
        {
            return is_running_;
        }

    private:
        std::atomic<bool> is_running_ { false };
        std::thread       thread_object_;
        ThreadFuncImpl    thread_runner_;
        ThreadFuncImpl    thread_closer_;
    };
}

#endif
