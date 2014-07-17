#include "Logger.h"
#include "Utilities.h"
#include "EventManager.h"
#include "TimerManager.h"
#include "SocketManager.h"
#include "ControllerManager.h"
#include "TcpSocket.h"
#include "UdpSocket.h"
#include "Timer.h"
#include "AsmHacks.h"

#include <windows.h>

#include <vector>
#include <memory>
#include <cassert>

#define LOG_FILE FOLDER "dll.log"

#define FRAME_INTERVAL  ( 1000 / 60 )

using namespace std;

struct Main : public Socket::Owner, public Timer::Owner, public ControllerManager::Owner
{
    HANDLE pipe;
    SocketPtr ipcSocket;
    Timer timer;
    bool timerFlag;

    // void acceptEvent ( Socket *serverSocket ) { serverSocket->accept ( this ).reset(); }

    void connectEvent ( Socket *socket )
    {
        LOG ( "Socket %08x connected", socket );
    }

    void disconnectEvent ( Socket *socket )
    {
        LOG ( "Socket %08x disconnected", socket );
    }

    void readEvent ( Socket *socket, const MsgPtr& msg, const IpAddrPort& address )
    {
        LOG ( "Got %s from '%s'; socket=%08x", msg, address, socket );

        // if ( msg->getMsgType() == MsgType::SocketShareData )
        // {
        //     if ( msg->getAs<SocketShareData>().isTCP() )
        //         sharedSocket = TcpSocket::shared ( this, msg->getAs<SocketShareData>() );
        //     else
        //         sharedSocket = UdpSocket::shared ( this, msg->getAs<SocketShareData>() );

        //     MsgPtr msg ( new IpAddrPort ( sharedSocket->getRemoteAddress() ) );
        //     ipcSocket->send ( msg, address );
        // }

        // assert ( false );
    }

    void timerExpired ( Timer *timer ) override
    {
        assert ( timer == &this->timer );

        timerFlag = true;
    }

    Main() : pipe ( 0 ), ipcSocket ( UdpSocket::bind ( this, 0 ) ), timer ( this ), timerFlag ( false )
    {
        LOG ( "Connecting pipe" );

        pipe = CreateFile (
                   NAMED_PIPE,                              // name of the pipe
                   GENERIC_READ | GENERIC_WRITE,            // 2-way pipe
                   FILE_SHARE_READ | FILE_SHARE_WRITE,      // R/W sharing mode
                   0,                                       // default security
                   OPEN_EXISTING,                           // open existing pipe
                   FILE_ATTRIBUTE_NORMAL,                   // default attributes
                   0 );                                     // no template file

        if ( pipe == INVALID_HANDLE_VALUE )
        {
            WindowsError err = GetLastError();
            LOG ( "CreateFile failed: %s", err );
            throw err;
        }

        LOG ( "Pipe connected" );

        DWORD bytes;

        if ( !WriteFile ( pipe, & ( ipcSocket->address.port ), sizeof ( ipcSocket->address.port ), &bytes, 0 ) )
        {
            WindowsError err = GetLastError();
            LOG ( "WriteFile failed: %s", err );
            throw err;
        }

        if ( bytes != sizeof ( ipcSocket->address.port ) )
        {
            LOG ( "WriteFile wrote %d bytes, expected %d", bytes, sizeof ( ipcSocket->address.port ) );
            throw "something"; // TODO
        }

        int processId = GetCurrentProcessId();

        if ( !WriteFile ( pipe, &processId, sizeof ( processId ), &bytes, 0 ) )
        {
            WindowsError err = GetLastError();
            LOG ( "WriteFile failed: %s", err );
            throw err;
        }

        if ( bytes != sizeof ( processId ) )
        {
            LOG ( "WriteFile wrote %d bytes, expected %d", bytes, sizeof ( processId ) );
            throw "something"; // TODO
        }
    }

    ~Main()
    {
        if ( pipe )
            CloseHandle ( pipe );
    }
};

static enum State { UNINITIALIZED, POLLING, STOPPING, DEINITIALIZED } state = UNINITIALIZED;

static shared_ptr<Main> main;

extern "C" void callback()
{
    try
    {
        if ( state == UNINITIALIZED )
        {
            // Joystick and timer must be initialized in the main thread
            TimerManager::get().initialize();
            ControllerManager::get().initialize ( main.get() );
            EventManager::get().startPolling();
            state = POLLING;
        }

        if ( state != POLLING )
            return;

        // main->timerFlag = false;
        // main->timer.start ( FRAME_INTERVAL );

        // // Poll for events
        // while ( !main->timerFlag )
        // {
        //     if ( !EventManager::get().poll() )
        //     {
        //         state = STOPPING;
        //         break;
        //     }
        // }
    }
    catch ( const WindowsError& err )
    {
        state = STOPPING;
    }
    catch ( ... )
    {
        LOG ( "Unknown exception!" );
        state = STOPPING;
    }

    if ( state == STOPPING )
    {
        EventManager::get().stop();
        ControllerManager::get().deinitialize();
        TimerManager::get().deinitialize();
        SocketManager::get().deinitialize();
        state = DEINITIALIZED;
        exit ( 0 );
    }
}

extern "C" BOOL APIENTRY DllMain ( HMODULE, DWORD reason, LPVOID )
{
    switch ( reason )
    {
        case DLL_PROCESS_ATTACH:
            Logger::get().initialize ( LOG_FILE );
            LOG ( "DLL_PROCESS_ATTACH" );
            try
            {
                SocketManager::get().initialize();

                main.reset ( new Main() );

                WindowsError err;

#define WRITE_ASM_HACK(ASM_HACK)                                \
    do {                                                        \
        if ( ( err = ASM_HACK.write() ).code != 0 ) {           \
            LOG ( "%s failed: %s", #ASM_HACK, err );            \
            exit ( 0 );                                         \
        }                                                       \
    } while ( 0 )

                static const Asm hookCallback1 =
                {
                    HOOK_CALL1_ADDR,
                    {
                        0xE8, INLINE_DWORD ( ( ( char * ) &callback ) - HOOK_CALL1_ADDR - 5 ),  // call callback
                        0xE9, INLINE_DWORD ( HOOK_CALL2_ADDR - HOOK_CALL1_ADDR - 10 )           // jmp HOOK_CALL2_ADDR
                    }
                };

                WRITE_ASM_HACK ( hookCallback1 );
                WRITE_ASM_HACK ( hookCallback2 );
                WRITE_ASM_HACK ( loopStartJump ); // Write the jump location last, due to dependencies on the above

                for ( void *const addr : disabledStageAddrs )
                {
                    Asm enableStage = { addr, INLINE_DWORD_FF };

                    if ( ( err = enableStage.write() ).code )
                    {
                        LOG ( "enableStage { %08x } failed: %s", addr, err );
                        exit ( 0 );
                    }
                }

                WRITE_ASM_HACK ( fixRyougiStageMusic1 );
                WRITE_ASM_HACK ( fixRyougiStageMusic2 );
            }
            catch ( const WindowsError& err )
            {
                exit ( 0 );
            }
            catch ( ... )
            {
                LOG ( "Unknown exception!" );
                exit ( 0 );
            }
            break;

        case DLL_PROCESS_DETACH:
            main.reset();
            LOG ( "DLL_PROCESS_DETACH" );
            EventManager::get().release();
            Logger::get().deinitialize();
            break;
    }

    return TRUE;
}
