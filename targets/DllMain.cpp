#include "Main.h"
#include "Thread.h"
#include "DllHacks.h"
#include "NetplayManager.h"
#include "ChangeMonitor.h"
#include "SmartSocket.h"
#include "Exceptions.h"
#include "Enum.h"
#include "ErrorStringsExt.h"
#include "KeyboardState.h"
#include "CharacterSelect.h"
#include "SpectatorManager.h"

#include <windows.h>

#include <vector>
#include <memory>
#include <algorithm>

using namespace std;


#define LOG_FILE                    FOLDER "dll.log"

#define DELAYED_STOP                ( 100 )

#define RESEND_INPUTS_INTERVAL      ( 100 )

#define VK_TOGGLE_OVERLAY           ( VK_F4 )


#define LOG_SYNC(FORMAT, ...)                                                                                   \
    LOG_TO ( syncLog, "%s [%u] %s [%s] " FORMAT,                                                                \
             gameModeStr ( *CC_GAME_MODE_ADDR ), *CC_GAME_MODE_ADDR,                                            \
             netMan.getState(), netMan.getIndexedFrame(), __VA_ARGS__ )


// Main application state
static ENUM ( AppState, Uninitialized, Polling, Stopping, Deinitialized ) appState = AppState::Uninitialized;

// Main application instance
struct DllMain;
static shared_ptr<DllMain> main;

// Mutex for deinitialize()
static Mutex deinitMutex;
static void deinitialize();

// Enum of variables to monitor
ENUM ( Variable, WorldTime, GameMode, RoundStart, SkippableFlag,
       MenuConfirmState, AutoReplaySave, GameStateCounter, CurrentMenuIndex );


struct DllMain
        : public Main
        , public RefChangeMonitor<Variable, uint32_t>::Owner
        , public PtrToRefChangeMonitor<Variable, uint32_t>::Owner
        , protected SpectatorManager
{
    // NetplayManager instance
    NetplayManager netMan;

    // If remote has loaded up to character select
    bool remoteCharaSelectLoaded = false;

    // ChangeMonitor for CC_WORLD_TIMER_ADDR
    RefChangeMonitor<Variable, uint32_t> worldTimerMoniter;

    // Timeout for each call to EventManager::poll
    // TODO figure out if there is any way to increase this, maybe dynamically?
    uint64_t pollTimeout = 1;

    // Local and remote player numbers
    uint8_t localPlayer = 1, remotePlayer = 2;

    // Timer for resending inputs while waiting
    TimerPtr resendTimer;

    // Indicates if we should sync the game RngState on this frame
    bool shouldSyncRngState = false;

    // Frame to stop on, when fast-forwarding the game.
    // Used as a flag to indicate fast-forward mode, 0:0 means not fast-forwarding.
    IndexedFrame fastFwdStopFrame = {{ 0, 0 }};

    // Initial connect timer
    TimerPtr initialTimer;

    // Controller mappings
    ControllerMappings mappings;

    // All controllers
    vector<Controller *> allControllers;

    // Player controllers
    array<Controller *, 2> playerControllers = {{ 0, 0 }};

    // Player controller selector positions
    array<size_t, 2> playerPositions = {{ 0, 0 }};

    // Local player inputs
    array<uint16_t, 2> localInputs = {{ 0, 0 }};

    // If we have sent our local retry menu index
    bool localRetryMenuIndexSent = false;

    // If we should disconnect at the next NetplayState change
    bool lazyDisconnect = false;

    // If the game is over now (after checking P1 and P2 health values)
    bool isGameOver = false;

    // If the delay and/or rollback should be changed
    bool shouldChangeDelayRollback = false;

    // Latest ChangeConfig for changing delay/rollback
    ChangeConfig changeConfig;

#ifndef RELEASE
    // Local and remote SyncHashes
    list<MsgPtr> localSync, remoteSync;
#endif

    void overlayUiControls()
    {
        // Check all controllers
        for ( Controller *controller : allControllers )
        {
            if ( ( controller->isJoystick() && isDirectionPressed ( controller, 6 ) )
                    || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_RIGHT ) ) )
            {
                // Move controller right
                if ( controller == playerControllers[0] )
                {
                    playerControllers[0]->cancelMapping();
                    playerControllers[0] = 0;
                }
                else if ( clientMode.isNetplay() && localPlayer == 1 )
                {
                    // Only one controller (player 1)
                    continue;
                }
                else if ( !playerControllers[1] )
                {
                    playerControllers[1] = controller;
                    playerPositions[1] = 0;
                }

            }
            else if ( ( controller->isJoystick() && isDirectionPressed ( controller, 4 ) )
                      || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_LEFT ) ) )
            {
                // Move controller left
                if ( controller == playerControllers[1] )
                {
                    playerControllers[1]->cancelMapping();
                    playerControllers[1] = 0;
                }
                else if ( clientMode.isNetplay() && localPlayer == 2 )
                {
                    // Only one controller (player 2)
                    continue;
                }
                else if ( !playerControllers[0] )
                {
                    playerControllers[0] = controller;
                    playerPositions[0] = 0;
                }
            }
        }

        array<string, 3> text;

        // Display all controllers
        text[2] = "Controllers\n";
        for ( const Controller *controller : allControllers )
            if ( controller != playerControllers[0] && controller != playerControllers[1] )
                text[2] += "\n" + controller->getName();

        const size_t controllersHeight = 3 + allControllers.size();

        // Update player controllers
        for ( uint8_t i = 0; i < 2; ++i )
        {
            // Hide / disable other player's overlay in netplay
            if ( clientMode.isNetplay() && localPlayer != i + 1 )
            {
                text[i].clear();
                DllHacks::updateSelector ( i );
                continue;
            }

            // Show placeholder when player has no controller assigned
            if ( !playerControllers[i] )
            {
                text[i] = ( i == 0 ? "Press left on P1 controller" : "Press right on P2 controller" );
                DllHacks::updateSelector ( i );
                continue;
            }

            // Generate mapping options starting with controller name
            size_t headerHeight = 0;
            vector<string> options;
            options.push_back ( playerControllers[i]->getName() );

            if ( playerControllers[i]->isKeyboard() )
            {
                headerHeight = max ( 3u, controllersHeight );

                // Instructions for mapping keyboard controls
                text[i] = "Press enter to set a direction key\n";
                text[i] += format ( "Press %s to delete a key\n", ( i == 0 ? "left" : "right" ) );
                text[i] += string ( headerHeight - 3, '\n' );

                // Add directions to keyboard mapping options
                for ( size_t j = 0; j < 4; ++j )
                {
                    const string mapping = playerControllers[i]->getMapping ( gameInputBits[j].second, "..." );
                    options.push_back ( gameInputBits[j].first + " : " + mapping );
                }
            }
            else
            {
                headerHeight = max ( 2u, controllersHeight );

                // Instructions for mapping joystick buttons
                text[i] = format ( "Press %s to delete a key\n", ( i == 0 ? "left" : "right" ) );
                text[i] += string ( headerHeight - 2, '\n' );
            }

            // Add buttons to mapping options
            for ( size_t j = 4; j < gameInputBits.size(); ++j )
            {
                const string mapping = playerControllers[i]->getMapping ( gameInputBits[j].second );
                options.push_back ( gameInputBits[j].first + " : " + mapping );
            }

            // Finally add done option
            options.push_back ( playerControllers[i]->isKeyboard() ? "Done (press enter)" : "Done (press any button)" );

            // Toggle overlay if both players are done
            if ( playerPositions[i] + 1 == options.size()
                    && ( ( playerControllers[i]->isJoystick() && isAnyButtonReleased ( playerControllers[i] ) )
                         || ( playerControllers[i]->isKeyboard() && KeyboardState::isReleased ( VK_RETURN ) ) ) )
            {
                playerPositions[i] = 0;

                if ( ( !playerControllers[0] || !playerPositions[0] )
                        && ( !playerControllers[1] || !playerPositions[1] ) )
                {
                    text[0] = text[1] = text[2] = "";
                    DllHacks::updateOverlay ( text );
                    DllHacks::toggleOverlay();
                    return;
                }
            }

            // Update overlay text with all the options
            for ( const string& option : options )
                text[i] += "\n" + option;

            // Filter keyboard overlay controls when mapping directions
            if ( playerControllers[i]->isKeyboard() && playerControllers[i]->isMapping()
                    && playerPositions[i] >= 1 && playerPositions[i] <= 4 )
            {
                DllHacks::updateSelector ( i, headerHeight + playerPositions[i], options[playerPositions[i]] );
                continue;
            }

            bool deleteMapping = false, mapDirections = false, changedPosition = false;

            if ( ( i == 0
                    && ( ( playerControllers[i]->isJoystick() && isDirectionPressed ( playerControllers[i], 4 ) )
                         || ( playerControllers[i]->isKeyboard() && KeyboardState::isPressed ( VK_LEFT ) ) ) )
                    || ( i == 1
                         && ( ( playerControllers[i]->isJoystick() && isDirectionPressed ( playerControllers[i], 6 ) )
                              || ( playerControllers[i]->isKeyboard() && KeyboardState::isPressed ( VK_RIGHT ) ) ) ) )
            {
                // Delete selected mapping
                deleteMapping = true;
            }
            else if ( playerControllers[i]->isKeyboard()
                      && ( KeyboardState::isReleased ( VK_RETURN ) || KeyboardState::isReleased ( VK_DELETE ) )
                      && playerPositions[i] >= 1 && playerPositions[i] <= 4 )
            {
                // Press enter / delete to modify direction keys
                if ( KeyboardState::isReleased ( VK_RETURN ) )
                    mapDirections = true;
                else
                    deleteMapping = true;
            }
            else if ( ( playerControllers[i]->isJoystick() && isDirectionPressed ( playerControllers[i], 2 ) )
                      || ( playerControllers[i]->isKeyboard() && KeyboardState::isPressed ( VK_DOWN ) ) )
            {
                // Move selector down
                playerPositions[i] = ( playerPositions[i] + 1 ) % options.size();
                changedPosition = true;
            }
            else if ( ( playerControllers[i]->isJoystick() && isDirectionPressed ( playerControllers[i], 8 ) )
                      || ( playerControllers[i]->isKeyboard() && KeyboardState::isPressed ( VK_UP ) ) )
            {
                // Move selector up
                playerPositions[i] = ( playerPositions[i] + options.size() - 1 ) % options.size();
                changedPosition = true;
            }

            if ( deleteMapping || mapDirections || changedPosition )
            {
                if ( playerPositions[i] >= 1 && playerPositions[i] < gameInputBits.size() + 1 )
                {
                    // Convert selector position to game input bit position
                    const size_t pos = playerPositions[i] - 1 + ( playerControllers[i]->isKeyboard() ? 0 : 4 );

                    ASSERT ( pos >= 0 );

                    if ( deleteMapping && pos < gameInputBits.size() )
                    {
                        // Delete mapping
                        playerControllers[i]->clearMapping ( gameInputBits[pos].second );
                        saveMappings ( playerControllers[i] );
                    }
                    else if ( pos >= 4 && pos < gameInputBits.size() )
                    {
                        // Map a button only
                        playerControllers[i]->startMapping ( this, gameInputBits[pos].second, DllHacks::windowHandle,
                        { VK_ESCAPE, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_TOGGLE_OVERLAY },
                        MAP_PRESERVE_DIRS | MAP_CONTINUOUSLY );
                    }
                    else if ( mapDirections && pos < 4 )
                    {
                        ASSERT ( playerControllers[i]->isKeyboard() == true );

                        // Map a keyboard direction
                        playerControllers[i]->startMapping ( this, gameInputBits[pos].second, DllHacks::windowHandle );
                    }
                    else
                    {
                        // In all other situations cancel the current mapping
                        playerControllers[i]->cancelMapping();
                    }
                }
                else
                {
                    // In all other situations cancel the current mapping
                    playerControllers[i]->cancelMapping();
                }
            }

            if ( playerPositions[i] == 0 )
            {
                text[i] = string ( "Press up or down to set keys" )
                          + string ( controllersHeight, '\n' )
                          + playerControllers[i]->getName();
                DllHacks::updateSelector ( i, controllersHeight, playerControllers[i]->getName() );
            }
            else
            {
                DllHacks::updateSelector ( i, headerHeight + playerPositions[i], options[playerPositions[i]] );
            }
        }

        DllHacks::updateOverlay ( text );
    }

    void frameStepNormal()
    {
        switch ( netMan.getState().value )
        {
            case NetplayState::PreInitial:
            case NetplayState::Initial:
            case NetplayState::AutoCharaSelect:
                // Disable FPS limit while going to character select
                *CC_SKIP_FRAMES_ADDR = 1;
                break;

            case NetplayState::InGame:
                // Only save rollback states in-game
                if ( netMan.isRollbackState() )
                    procMan.saveState ( netMan );

            case NetplayState::CharaSelect:
            case NetplayState::Loading:
            case NetplayState::Skippable:
            case NetplayState::RetryMenu:
            {
                // Fast forward if spectator
                if ( clientMode.isSpectate() && netMan.getState() != NetplayState::Loading )
                {
                    static bool doneSkipping = true;

                    const IndexedFrame remoteIndexedFrame = netMan.getRemoteIndexedFrame();

                    if ( doneSkipping && remoteIndexedFrame.value > netMan.getIndexedFrame().value + NUM_INPUTS )
                    {
                        if ( remoteIndexedFrame.parts.index > netMan.getIndex() )
                            *CC_SKIP_FRAMES_ADDR = 16;
                        else
                            *CC_SKIP_FRAMES_ADDR = 4;
                        doneSkipping = false;
                    }
                    else if ( !doneSkipping && *CC_SKIP_FRAMES_ADDR == 0 )
                    {
                        doneSkipping = true;
                    }
                }

                KeyboardState::update();
                ControllerManager::get().check ( DllHacks::windowHandle );

                ASSERT ( localPlayer == 1 || localPlayer == 2 );

                bool toggleOverlay = false;

                if ( KeyboardState::isPressed ( VK_TOGGLE_OVERLAY ) )
                    toggleOverlay = true;

                for ( Controller *controller : allControllers )
                {
                    if ( ( netMan.getState().value == NetplayState::CharaSelect || clientMode.isNetplay() )
                            && controller->isJoystick() && isButtonPressed ( controller, CC_BUTTON_START ) )
                    {
                        toggleOverlay = true;
                    }

                    if ( DllHacks::isOverlayEnabled() )
                        continue;

                    uint16_t input = getInput ( controller );

                    // Sticky controllers to the first available player when anything is pressed EXCEPT start
                    if ( input && ! ( input & COMBINE_INPUT ( 0, CC_BUTTON_START ) ) )
                    {
                        if ( clientMode.isNetplay() )
                        {
                            // Only sticky the local player in netplay
                            if ( !playerControllers[localPlayer - 1]
                                    && controller != playerControllers[localPlayer - 1] )
                            {
                                playerControllers[localPlayer - 1] = controller;
                            }
                        }
                        else
                        {
                            if ( !playerControllers[0] && controller != playerControllers[1] )
                                playerControllers[0] = controller;
                            else if ( !playerControllers[1] && controller != playerControllers[0] )
                                playerControllers[1] = controller;
                        }
                    }
                }

                // Only toggle overlay if both players are "done"; ie at the first option
                if ( toggleOverlay && !playerPositions[0] && !playerPositions[1] )
                    DllHacks::toggleOverlay();

                if ( DllHacks::isOverlayEnabled() )             // Overlay UI input
                {
                    overlayUiControls();
                    localInputs[0] = localInputs[1] = 0;
                }
                else if ( clientMode.isNetplay() )              // Netplay input
                {
                    if ( playerControllers[localPlayer - 1] )
                        localInputs[0] = getInput ( playerControllers[localPlayer - 1] );

                    if ( GetKeyState ( VK_CONTROL ) & 0x80 )
                    {
                        for ( uint8_t i = 0; i < 10; ++i )
                        {
                            // Offset by -1, so Ctrl+0 sets delay 10
                            const uint8_t delay = 1 + ( i + 9 ) % 10;

                            if ( delay == netMan.getDelay() )
                                continue;

                            if ( GetKeyState ( '0' + i ) & 0x80 )
                            {
                                shouldChangeDelayRollback = true;
                                changeConfig.indexedFrame = netMan.getIndexedFrame();
                                changeConfig.delay = delay;
                                dataSocket->send ( changeConfig );
                                break;
                            }
                        }
                    }

#ifndef RELEASE
                    // Test random delay setting
                    static bool randomize = false;

                    if ( KeyboardState::isPressed ( VK_F11 ) )
                        randomize = !randomize;

                    if ( randomize && rand() % 5 == 0 )
                    {
                        shouldChangeDelayRollback = true;
                        changeConfig.indexedFrame = netMan.getIndexedFrame();
                        changeConfig.delay = 1 + rand() % 9;
                        dataSocket->send ( changeConfig );
                    }
#endif
                }
                else if ( clientMode.isLocal() )                // Local input
                {
                    if ( playerControllers[0] )
                        localInputs[0] = getInput ( playerControllers[0] );
                }
                else if ( clientMode.isSpectate() )             // Spectator input
                {
                    if ( playerControllers[0] && playerControllers[0]->getState() )
                        *CC_SKIP_FRAMES_ADDR = 0;
                }
                else
                {
                    LOG ( "Unknown clientMode=%s; flags={ %s }", clientMode, clientMode.flagString() );
                    break;
                }

#ifndef RELEASE
                // Test random input
                static bool randomize = false;

                if ( KeyboardState::isPressed ( VK_F12 ) )
                    randomize = !randomize;

                if ( randomize )
                {
                    if ( netMan.getFrame() % 2 )
                    {
                        uint16_t direction = ( rand() % 10 );

                        // Reduce the chances of moving the cursor at retry menu
                        if ( netMan.getState().value == NetplayState::RetryMenu && ( rand() % 2 ) )
                            direction = 0;

                        uint16_t buttons = ( rand() % 0x1000 );

                        // Prevent hitting some non-essential buttons
                        buttons &= ~ ( CC_BUTTON_D | CC_BUTTON_FN1 | CC_BUTTON_FN2 | CC_BUTTON_START );

                        // Prevent going back at character select
                        if ( netMan.getState().value == NetplayState::CharaSelect )
                            buttons &= ~ ( CC_BUTTON_B | CC_BUTTON_CANCEL );

                        if ( clientMode.isLocal() )
                            localInputs[1] = COMBINE_INPUT ( direction, buttons );
                        else
                            localInputs[0] = COMBINE_INPUT ( direction, buttons );
                    }
                }
#endif

                // Assign local player input
                if ( !clientMode.isSpectate() )
                    netMan.setInput ( localPlayer, localInputs[0] );

                if ( clientMode.isNetplay() )
                {
                    // Special netplay retry menu behaviour, only select final option after both sides have selected
                    if ( netMan.getState().value == NetplayState::RetryMenu )
                    {
                        MsgPtr msgMenuIndex = netMan.getLocalRetryMenuIndex();

                        // Lazy disconnect now once the retry menu option has been selected
                        if ( msgMenuIndex && ( !dataSocket || !dataSocket->isConnected() ) )
                        {
                            if ( lazyDisconnect )
                            {
                                lazyDisconnect = false;
                                delayedStop ( "Disconnected!" );
                            }
                            break;
                        }

                        // Only send retry menu index once
                        if ( msgMenuIndex && !localRetryMenuIndexSent )
                        {
                            localRetryMenuIndexSent = true;
                            dataSocket->send ( msgMenuIndex );
                        }
                        break;
                    }

                    dataSocket->send ( netMan.getInputs ( localPlayer ) );
                }
                else if ( clientMode.isLocal() )
                {
                    if ( playerControllers[1] && !DllHacks::isOverlayEnabled() )
                        localInputs[1] = getInput ( playerControllers[1] );

                    netMan.setInput ( remotePlayer, localInputs[1] );
                }

                if ( shouldSyncRngState && ( clientMode.isHost() || clientMode.isBroadcast() ) )
                {
                    shouldSyncRngState = false;

                    MsgPtr msgRngState = procMan.getRngState ( netMan.getIndex() );

                    ASSERT ( msgRngState.get() != 0 );

                    netMan.setRngState ( msgRngState->getAs<RngState>() );

                    if ( clientMode.isHost() )
                        dataSocket->send ( msgRngState );
                }
                break;
            }

            default:
                ASSERT ( !"Unknown NetplayState!" );
                break;
        }

        // // Clear the last changed frame before we get new inputs
        // netMan.clearLastChangedFrame();

        for ( ;; )
        {
            // Poll until we are ready to run
            if ( !EventManager::get().poll ( pollTimeout ) )
            {
                appState = AppState::Stopping;
                return;
            }

            // Don't need to wait for anything in local modes
            if ( clientMode.isLocal() || lazyDisconnect )
                break;

            // Check if we are ready to continue running, ie not waiting on remote input or RngState
            const bool ready = ( netMan.isRemoteInputReady() && netMan.isRngStateReady ( shouldSyncRngState ) );

            // Don't resend inputs in spectator mode
            if ( clientMode.isSpectate() )
            {
                if ( ready )
                    break;

                // Just keep polling if not ready
                continue;
            }

            // Stop resending inputs if we're ready
            if ( ready )
            {
                resendTimer.reset();
                break;
            }

            // Start resending inputs since we are waiting
            if ( !resendTimer )
            {
                resendTimer.reset ( new Timer ( this ) );
                resendTimer->start ( RESEND_INPUTS_INTERVAL );
            }
        }

        // // Only do rollback related stuff while in-game
        // if ( netMan.getState() == NetplayState::InGame && netMan.isRollbackState()
        //         && netMan.getLastChangedFrame().value < netMan.getIndexedFrame().value )
        // {
        //     LOG_SYNC ( "Rollback: %s -> %s", netMan.getIndexedFrame(), netMan.getLastChangedFrame() );

        //     // Indicate we're re-running (save the frame first)
        //     fastFwdStopFrame = netMan.getIndexedFrame();

        //     // Reset the game state (this resets game state and netMan state)
        //     procMan.loadState ( netMan.getLastChangedFrame(), netMan );
        //     return;
        // }

        // Update the RngState if necessary
        if ( shouldSyncRngState )
        {
            shouldSyncRngState = false;

            // LOG ( "Randomizing RngState" );

            // RngState rngState;

            // rngState.rngState0 = rand() % 1000000;
            // rngState.rngState1 = rand() % 1000000;
            // rngState.rngState2 = rand() % 1000000;

            // for ( char& c : rngState.rngState3 )
            //     c = rand() % 256;

            // procMan.setRngState ( rngState );

            MsgPtr msgRngState = netMan.getRngState();

            if ( msgRngState )
                procMan.setRngState ( msgRngState->getAs<RngState>() );
        }

        // Update delay and/or rollback if necessary
        if ( shouldChangeDelayRollback )
        {
            shouldChangeDelayRollback = false;

            if ( changeConfig.delay != 0xFF )
            {
                netMan.setDelay ( changeConfig.delay );
                // netMan.setRollback ( changeConfig.rollback );
            }
        }

#ifndef RELEASE
        // Log the RngState once every 5 seconds after CharaSelect, except in Loading, Skippable, and RetryMenu states.
        // This effectively also logs whenever the frame becomes zero, ie when the index is incremented.
        if ( dataSocket && dataSocket->isConnected() && netMan.getFrame() % ( 5 * 60 ) == 0
                && netMan.getState().value >= NetplayState::CharaSelect && netMan.getState() != NetplayState::Loading
                && netMan.getState() != NetplayState::Skippable && netMan.getState() != NetplayState::RetryMenu )
        {
            MsgPtr msgRngState = procMan.getRngState ( netMan.getIndex() );

            ASSERT ( msgRngState.get() != 0 );

            LOG_SYNC ( "RngState: %s", msgRngState->getAs<RngState>().dump() );

            // Check for desyncs by periodically sending hashes
            MsgPtr msgSyncHash ( new SyncHash ( netMan.getIndexedFrame(), msgRngState->getAs<RngState>() ) );

            dataSocket->send ( msgSyncHash );

            localSync.push_back ( msgSyncHash );

            while ( !localSync.empty() && !remoteSync.empty() )
            {
                if ( localSync.front()->getAs<SyncHash>() == remoteSync.front()->getAs<SyncHash>() )
                {
                    localSync.pop_front();
                    remoteSync.pop_front();
                    continue;
                }

                LOG_SYNC ( "Desync: local=[%s]; remote=[%s]",
                           localSync.front()->getAs<SyncHash>().indexedFrame,
                           remoteSync.front()->getAs<SyncHash>().indexedFrame );

                delayedStop ( "Desync!" );
                return;
            }
        }
#endif

        // Log inputs every frame
        LOG_SYNC ( "Inputs: %04x %04x", netMan.getRawInput ( 1 ), netMan.getRawInput ( 2 ) );
    }

    void frameStepRerun()
    {
        // Stop fast-forwarding once we're reached the frame we want
        if ( netMan.getIndexedFrame().value >= fastFwdStopFrame.value )
            fastFwdStopFrame.value = 0;

        // Disable FPS limit while fast-forwarding
        if ( fastFwdStopFrame.value )
            *CC_SKIP_FRAMES_ADDR = 1;
    }

    void frameStep()
    {
        // New frame
        netMan.updateFrame();
        procMan.clearInputs();

        // Check for changes to important variables for state transitions
        ChangeMonitor::get().check();

        // Perform the frame step
        if ( fastFwdStopFrame.value )
            frameStepRerun();
        else
            frameStepNormal();

        // Update spectators
        frameStepSpectators();

        // Write game inputs
        procMan.writeGameInput ( localPlayer, netMan.getInput ( localPlayer ) );
        procMan.writeGameInput ( remotePlayer, netMan.getInput ( remotePlayer ) );
    }

    void netplayStateChanged ( NetplayState state )
    {
        ASSERT ( netMan.getState() != state );

        DllHacks::disableOverlay();

        // Entering InGame
        if ( state == NetplayState::InGame )
        {
            if ( netMan.isRollbackState() )
                procMan.allocateStates();
        }

        // Exiting InGame
        if ( state != NetplayState::InGame )
        {
            if ( netMan.config.rollback )
                procMan.deallocateStates();
        }

        // Entering CharaSelect OR entering InGame
        if ( !clientMode.isOffline() && ( state == NetplayState::CharaSelect || state == NetplayState::InGame ) )
        {
            shouldSyncRngState = !isGameOver;
            LOG ( "[%s] shouldSyncRngState=%u", netMan.getIndexedFrame(), shouldSyncRngState );
        }

        // Entering RetryMenu (enable lazy disconnect on netplay)
        if ( state == NetplayState::RetryMenu )
        {
            lazyDisconnect = clientMode.isNetplay();

            // Clear state flags
            isGameOver = false;
            wasLastRoundDoubleGamePointDraw = false;
            localRetryMenuIndexSent = false;
        }
        else if ( lazyDisconnect )
        {
            lazyDisconnect = false;

            if ( !dataSocket || !dataSocket->isConnected() )
            {
                delayedStop ( "Disconnected!" );
                return;
            }
        }

        netMan.setState ( state );
    }

    void gameModeChanged ( uint32_t previous, uint32_t current )
    {
        if ( current == 0
                || current == CC_GAME_MODE_STARTUP
                || current == CC_GAME_MODE_OPENING
                || current == CC_GAME_MODE_TITLE
                || current == CC_GAME_MODE_MAIN
                || current == CC_GAME_MODE_LOADING_DEMO
                || ( previous == CC_GAME_MODE_LOADING_DEMO && current == CC_GAME_MODE_INGAME )
                || current == CC_GAME_MODE_HIGH_SCORES )
        {
            ASSERT ( netMan.getState() == NetplayState::PreInitial || netMan.getState() == NetplayState::Initial );
            return;
        }

        if ( netMan.getState() == NetplayState::Initial && netMan.config.mode.isSpectate()
                && netMan.initial.netplayState > NetplayState::CharaSelect )
        {
            // Spectate mode needs to auto select characters if starting after CharaSelect
            netplayStateChanged ( NetplayState::AutoCharaSelect );
            return;
        }

        if ( current == CC_GAME_MODE_CHARA_SELECT )
        {
            netplayStateChanged ( NetplayState::CharaSelect );
            return;
        }

        if ( current == CC_GAME_MODE_LOADING )
        {
            netplayStateChanged ( NetplayState::Loading );
            return;
        }

        if ( current == CC_GAME_MODE_INGAME )
        {
            // Versus mode in-game starts with character intros, which is a skippable state
            if ( netMan.config.mode.isVersus() )
                netplayStateChanged ( NetplayState::Skippable );
            else
                netplayStateChanged ( NetplayState::InGame );
            return;
        }

        if ( current == CC_GAME_MODE_RETRY )
        {
            netplayStateChanged ( NetplayState::RetryMenu );
            return;
        }

        THROW_EXCEPTION ( "gameModeChanged(%u, %u)", ERROR_INVALID_GAME_MODE, previous, current );
    }

    void delayedStop ( string error )
    {
        if ( !error.empty() )
            procMan.ipcSend ( new ErrorMessage ( error ) );

        stopTimer.reset ( new Timer ( this ) );
        stopTimer->start ( DELAYED_STOP );
    }

    // ChangeMonitor callback
    void hasChanged ( Variable var, uint32_t previous, uint32_t current ) override
    {
        switch ( var.value )
        {
            case Variable::WorldTime:
                frameStep();
                break;

            case Variable::GameMode:
                LOG ( "[%s] %s: previous=%u; current=%u", netMan.getIndexedFrame(), var, previous, current );
                gameModeChanged ( previous, current );
                break;

            case Variable::RoundStart:
                // In-game happens after round start, when players can start moving
                LOG ( "[%s] %s: previous=%u; current=%u", netMan.getIndexedFrame(), var, previous, current );
                netplayStateChanged ( NetplayState::InGame );
                break;

            case Variable::SkippableFlag:
                if ( clientMode.isTraining()
                        || ! ( previous == 0 && current == 1 && netMan.getState() == NetplayState::InGame ) )
                    break;

                // Check for game over since we just entered a skippable state
                updateGameOverFlags();

                // When the SkippableFlag is set while InGame (not training mode), we are in a Skippable state
                LOG ( "[%s] %s: previous=%u; current=%u", netMan.getIndexedFrame(), var, previous, current );
                netplayStateChanged ( NetplayState::Skippable );

                // Enable lazy disconnect if someone just won a game (netplay only)
                lazyDisconnect = ( clientMode.isNetplay() && isGameOver );
                break;

            default:
                LOG ( "[%s] %s: previous=%u; current=%u", netMan.getIndexedFrame(), var, previous, current );
                break;
        }
    }

    // Socket callbacks
    void acceptEvent ( Socket *serverSocket ) override
    {
        LOG ( "acceptEvent ( %08x )", serverSocket );

        if ( serverSocket == serverCtrlSocket.get() )
        {
            LOG ( "serverCtrlSocket->accept ( this )" );

            SocketPtr newSocket = serverCtrlSocket->accept ( this );

            LOG ( "newSocket=%08x", newSocket.get() );

            ASSERT ( newSocket != 0 );
            ASSERT ( newSocket->isConnected() == true );

            newSocket->send ( new VersionConfig ( clientMode ) );

            pushPendingSocket ( this, newSocket );
        }
        else if ( serverSocket == serverDataSocket.get() && !dataSocket )
        {
            LOG ( "serverDataSocket->accept ( this )" );

            dataSocket = serverDataSocket->accept ( this );

            LOG ( "dataSocket=%08x", dataSocket.get() );

            ASSERT ( dataSocket != 0 );
            ASSERT ( dataSocket->isConnected() == true );

            netplayStateChanged ( NetplayState::Initial );

            initialTimer.reset();
        }
        else
        {
            LOG ( "Unexpected acceptEvent from serverSocket=%08x", serverSocket );
            serverSocket->accept ( 0 ).reset();
            return;
        }
    }

    void connectEvent ( Socket *socket ) override
    {
        LOG ( "connectEvent ( %08x )", socket );

        ASSERT ( dataSocket.get() != 0 );
        ASSERT ( dataSocket->isConnected() == true );

        netplayStateChanged ( NetplayState::Initial );

        initialTimer.reset();

        SetForegroundWindow ( ( HWND ) DllHacks::windowHandle );
    }

    void disconnectEvent ( Socket *socket ) override
    {
        LOG ( "disconnectEvent ( %08x )", socket );

        if ( socket == dataSocket.get() )
        {
            if ( netMan.getState() == NetplayState::PreInitial )
            {
                dataSocket = SmartSocket::connectUDP ( this, address );
                LOG ( "dataSocket=%08x", dataSocket.get() );
                return;
            }

            if ( lazyDisconnect )
                return;

            delayedStop ( "Disconnected!" );
            return;
        }

        popPendingSocket ( socket );
        popSpectator ( socket );
    }

    void readEvent ( Socket *socket, const MsgPtr& msg, const IpAddrPort& address ) override
    {
        LOG ( "readEvent ( %08x, %s, %s )", socket, msg, address );

        if ( !msg.get() )
            return;

        switch ( msg->getMsgType() )
        {
            case MsgType::VersionConfig:
            {
                const Version RemoteVersion = msg->getAs<VersionConfig>().version;

                if ( !LocalVersion.similar ( RemoteVersion, 1 + options[Options::StrictVersion] ) )
                {
                    string local = LocalVersion.code;
                    string remote = RemoteVersion.code;

                    if ( options[Options::StrictVersion] >= 2 )
                    {
                        local += " " + LocalVersion.commitId;
                        remote += " " + RemoteVersion.commitId;
                    }

                    if ( options[Options::StrictVersion] >= 3 )
                    {
                        local += " " + LocalVersion.buildTime;
                        remote += " " + RemoteVersion.buildTime;
                    }

                    LOG ( "Incompatible versions:\nLocal version: %s\nRemote version: %s", local, remote );

                    socket->disconnect();
                    return;
                }

                socket->send ( new SpectateConfig ( netMan.config, netMan.getState().value ) );
                return;
            }

            case MsgType::ConfirmConfig:
                pushSpectator ( socket );
                return;

            case MsgType::RngState:
                netMan.setRngState ( msg->getAs<RngState>() );
                return;

#ifndef RELEASE
            case MsgType::SyncHash:
                remoteSync.push_back ( msg );
                return;
#endif

            default:
                break;
        }

        switch ( clientMode.value )
        {
            case ClientMode::Host:
            case ClientMode::Client:
                switch ( msg->getMsgType() )
                {
                    case MsgType::PlayerInputs:
                        netMan.setInputs ( remotePlayer, msg->getAs<PlayerInputs>() );
                        return;

                    case MsgType::MenuIndex:
                        if ( netMan.getState() == NetplayState::RetryMenu )
                            netMan.setRemoteRetryMenuIndex ( msg->getAs<MenuIndex>().menuIndex );
                        return;

                    case MsgType::ChangeConfig:
                        if ( ( msg->getAs<ChangeConfig>().indexedFrame.value > changeConfig.indexedFrame.value )
                                || ( msg->getAs<ChangeConfig>().indexedFrame.value == changeConfig.indexedFrame.value
                                     && clientMode.isClient() ) )
                        {
                            shouldChangeDelayRollback = true;
                            changeConfig = msg->getAs<ChangeConfig>();
                        }
                        return;

                    default:
                        break;
                }
                break;

            case ClientMode::SpectateNetplay:
            case ClientMode::SpectateBroadcast:
                switch ( msg->getMsgType() )
                {
                    case MsgType::InitialGameState:
                        netMan.initial = msg->getAs<InitialGameState>();

                        if ( netMan.initial.chara[0] == UNKNOWN_POSITION )
                            THROW_EXCEPTION ( "chara[0]=Unknown", ERROR_INVALID_HOST_CONFIG );

                        if ( netMan.initial.chara[1] == UNKNOWN_POSITION )
                            THROW_EXCEPTION ( "chara[1]=Unknown", ERROR_INVALID_HOST_CONFIG );

                        if ( netMan.initial.moon[0] == UNKNOWN_POSITION )
                            THROW_EXCEPTION ( "moon[0]=Unknown", ERROR_INVALID_HOST_CONFIG );

                        if ( netMan.initial.moon[1] == UNKNOWN_POSITION )
                            THROW_EXCEPTION ( "moon[1]=Unknown", ERROR_INVALID_HOST_CONFIG );

                        LOG ( "InitialGameState: %s; indexedFrame=[%s]; stage=%u; isTraining=%u; %s vs %s",
                              NetplayState ( ( NetplayState::Enum ) netMan.initial.netplayState ),
                              netMan.initial.indexedFrame, netMan.initial.stage, netMan.initial.isTraining,
                              netMan.initial.formatCharaName ( 1, getFullCharaName ),
                              netMan.initial.formatCharaName ( 2, getFullCharaName ) );

                        netplayStateChanged ( NetplayState::Initial );
                        return;

                    case MsgType::BothInputs:
                        netMan.setBothInputs ( msg->getAs<BothInputs>() );
                        return;

                    case MsgType::MenuIndex:
                        netMan.setRetryMenuIndex ( msg->getAs<MenuIndex>().index, msg->getAs<MenuIndex>().menuIndex );
                        return;

                    default:
                        break;
                }
                break;

            default:
                break;
        }

        LOG ( "Unexpected '%s' from socket=%08x", msg, socket );
    }

    // ProcessManager callbacks
    void ipcConnectEvent() override
    {
    }

    void ipcDisconnectEvent() override
    {
        appState = AppState::Stopping;
        EventManager::get().stop();
    }

    void ipcReadEvent ( const MsgPtr& msg ) override
    {
        if ( !msg.get() )
            return;

        switch ( msg->getMsgType() )
        {
            case MsgType::OptionsMessage:
                options = msg->getAs<OptionsMessage>();

                Logger::get().sessionId = options.arg ( Options::SessionId );
                Logger::get().initialize ( options.arg ( Options::AppDir ) + LOG_FILE );

                LOG ( "SessionId '%s'", Logger::get().sessionId );

                syncLog.sessionId = options.arg ( Options::SessionId );
                syncLog.initialize ( options.arg ( Options::AppDir ) + SYNC_LOG_FILE, LOG_VERSION );
                break;

            case MsgType::ControllerMappings:
                mappings = msg->getAs<ControllerMappings>();
                KeyboardState::clear();
                ControllerManager::get().owner = this;
                ControllerManager::get().getKeyboard()->setMappings ( ProcessManager::fetchKeyboardConfig() );
                ControllerManager::get().setMappings ( mappings );
                ControllerManager::get().check();
                allControllers = ControllerManager::get().getControllers();
                break;

            case MsgType::ClientMode:
                if ( clientMode != ClientMode::Unknown )
                    break;

                clientMode = msg->getAs<ClientMode>();
                clientMode.flags |= ClientMode::GameStarted;
                LOG ( "%s: flags={ %s }", clientMode, clientMode.flagString() );
                break;

            case MsgType::IpAddrPort:
                if ( !address.empty() )
                    break;

                address = msg->getAs<IpAddrPort>();
                LOG ( "address='%s'", address );
                break;

            case MsgType::SpectateConfig:
                ASSERT ( clientMode.isSpectate() == true );

                netMan.config.mode       = clientMode;
                netMan.config.mode.flags |= msg->getAs<SpectateConfig>().mode.flags;
                netMan.config.sessionId  = Logger::get().sessionId;
                netMan.config.delay      = msg->getAs<SpectateConfig>().delay;
                netMan.config.rollback   = msg->getAs<SpectateConfig>().rollback;
                netMan.config.winCount   = msg->getAs<SpectateConfig>().winCount;
                netMan.config.hostPlayer = msg->getAs<SpectateConfig>().hostPlayer;
                netMan.config.names      = msg->getAs<SpectateConfig>().names;
                netMan.config.sessionId  = msg->getAs<SpectateConfig>().sessionId;

                if ( netMan.config.delay == 0xFF )
                    THROW_EXCEPTION ( "delay=%u", ERROR_INVALID_HOST_CONFIG, netMan.config.delay );

                netMan.initial = msg->getAs<SpectateConfig>().initial;

                if ( netMan.initial.netplayState == NetplayState::Unknown )
                    THROW_EXCEPTION ( "netplayState=NetplayState::Unknown", ERROR_INVALID_HOST_CONFIG );

                if ( netMan.initial.chara[0] == UNKNOWN_POSITION )
                    THROW_EXCEPTION ( "chara[0]=Unknown", ERROR_INVALID_HOST_CONFIG );

                if ( netMan.initial.chara[1] == UNKNOWN_POSITION )
                    THROW_EXCEPTION ( "chara[1]=Unknown", ERROR_INVALID_HOST_CONFIG );

                if ( netMan.initial.moon[0] == UNKNOWN_POSITION )
                    THROW_EXCEPTION ( "moon[0]=Unknown", ERROR_INVALID_HOST_CONFIG );

                if ( netMan.initial.moon[1] == UNKNOWN_POSITION )
                    THROW_EXCEPTION ( "moon[1]=Unknown", ERROR_INVALID_HOST_CONFIG );

                LOG ( "SpectateConfig: %s; flags={ %s }; delay=%d; rollback=%d; winCount=%d; hostPlayer=%u; "
                      "names={ '%s', '%s' }", netMan.config.mode, netMan.config.mode.flagString(), netMan.config.delay,
                      netMan.config.rollback, netMan.config.winCount, netMan.config.hostPlayer,
                      netMan.config.names[0], netMan.config.names[1] );

                LOG ( "InitialGameState: %s; stage=%u; isTraining=%u; %s vs %s",
                      NetplayState ( ( NetplayState::Enum ) netMan.initial.netplayState ),
                      netMan.initial.stage, netMan.initial.isTraining,
                      msg->getAs<SpectateConfig>().formatPlayer ( 1, getFullCharaName ),
                      msg->getAs<SpectateConfig>().formatPlayer ( 2, getFullCharaName ) );

                // Wait for final InitialGameState message before going to NetplayState::Initial
                break;

            case MsgType::NetplayConfig:
                if ( netMan.config.delay != 0xFF )
                    break;

                netMan.config = msg->getAs<NetplayConfig>();
                netMan.config.mode = clientMode;
                netMan.config.sessionId = Logger::get().sessionId;

                if ( netMan.config.delay == 0xFF )
                    THROW_EXCEPTION ( "delay=%u", ERROR_INVALID_HOST_CONFIG, netMan.config.delay );

                if ( clientMode.isNetplay() )
                {
                    if ( netMan.config.hostPlayer != 1 && netMan.config.hostPlayer != 2 )
                        THROW_EXCEPTION ( "hostPlayer=%u", ERROR_INVALID_HOST_CONFIG, netMan.config.hostPlayer );

                    // Determine the player numbers
                    if ( clientMode.isHost() )
                    {
                        localPlayer = netMan.config.hostPlayer;
                        remotePlayer = ( 3 - netMan.config.hostPlayer );
                    }
                    else
                    {
                        remotePlayer = netMan.config.hostPlayer;
                        localPlayer = ( 3 - netMan.config.hostPlayer );
                    }

                    netMan.setRemotePlayer ( remotePlayer );

                    if ( clientMode.isHost() )
                    {
                        serverCtrlSocket = SmartSocket::listenTCP ( this, address.port );
                        LOG ( "serverCtrlSocket=%08x", serverCtrlSocket.get() );

                        serverDataSocket = SmartSocket::listenUDP ( this, address.port );
                        LOG ( "serverDataSocket=%08x", serverDataSocket.get() );
                    }
                    else if ( clientMode.isClient() )
                    {
                        serverCtrlSocket = SmartSocket::listenTCP ( this, 0 );
                        LOG ( "serverCtrlSocket=%08x", serverCtrlSocket.get() );

                        // TODO send serverCtrlSocket->address.port to the host, for spectator delegation

                        dataSocket = SmartSocket::connectUDP ( this, address, clientMode.isUdpTunnel() );
                        LOG ( "dataSocket=%08x", dataSocket.get() );
                    }

                    initialTimer.reset ( new Timer ( this ) );
                    initialTimer->start ( DEFAULT_PENDING_TIMEOUT );

                    // Wait for dataSocket to be connected before changing to NetplayState::Initial
                }
                else if ( clientMode.isBroadcast() )
                {
                    ASSERT ( netMan.config.mode.isBroadcast() == true );

                    LOG ( "NetplayConfig: broadcastPort=%u", netMan.config.broadcastPort );

                    serverCtrlSocket = SmartSocket::listenTCP ( this, netMan.config.broadcastPort );
                    LOG ( "serverCtrlSocket=%08x", serverCtrlSocket.get() );

                    // Update the broadcast port and send over IPC
                    netMan.config.broadcastPort = serverCtrlSocket->address.port;
                    netMan.config.invalidate();

                    procMan.ipcSend ( netMan.config );

                    netplayStateChanged ( NetplayState::Initial );
                }
                else if ( clientMode.isOffline() )
                {
                    netplayStateChanged ( NetplayState::Initial );
                }

                *CC_DAMAGE_LEVEL_ADDR = 2;
                *CC_TIMER_SPEED_ADDR = 2;
                *CC_WIN_COUNT_VS_ADDR = ( uint32_t ) netMan.config.winCount;

                // *CC_WIN_COUNT_VS_ADDR = 1;
                // *CC_DAMAGE_LEVEL_ADDR = 4;

                LOG ( "SessionId '%s'", netMan.config.sessionId );

                LOG ( "NetplayConfig: %s; flags={ %s }; delay=%d; rollback=%d; winCount=%d; "
                      "hostPlayer=%d; localPlayer=%d; remotePlayer=%d",
                      netMan.config.mode, netMan.config.mode.flagString(), netMan.config.delay, netMan.config.rollback,
                      netMan.config.winCount, netMan.config.hostPlayer, localPlayer, remotePlayer );
                break;

            default:
                if ( clientMode.isSpectate() )
                {
                    readEvent ( 0, msg, NullAddress );
                    break;
                }

                LOG ( "Unexpected '%s'", msg );
                break;
        }
    }

    // ControllerManager callbacks
    void attachedJoystick ( Controller *controller ) override
    {
        allControllers.push_back ( controller );
    }

    void detachedJoystick ( Controller *controller ) override
    {
        if ( playerControllers[0] == controller )
            playerControllers[0] = 0;

        if ( playerControllers[1] == controller )
            playerControllers[1] = 0;

        auto it = find ( allControllers.begin(), allControllers.end(), controller );

        ASSERT ( it != allControllers.end() );

        allControllers.erase ( it );
    }

    // Controller callback
    void doneMapping ( Controller *controller, uint32_t key ) override
    {
        LOG ( "%s: controller=%08x; key=%08x", controller->getName(), controller, key );

        if ( key )
            saveMappings ( controller );
    }

    // Timer callback
    void timerExpired ( Timer *timer ) override
    {
        if ( timer == resendTimer.get() )
        {
            dataSocket->send ( netMan.getInputs ( localPlayer ) );
            resendTimer->start ( RESEND_INPUTS_INTERVAL );
        }
        else if ( timer == initialTimer.get() )
        {
            main->procMan.ipcSend ( new ErrorMessage ( "Disconnected!" ) );
            delayedStop ( "Disconnected!" );
            initialTimer.reset();
        }
        else if ( timer == stopTimer.get() )
        {
            appState = AppState::Stopping;
            EventManager::get().stop();
        }
        else
        {
            SpectatorManager::timerExpired ( timer );
        }
    }

    // DLL callback
    void callback()
    {
        // Don't poll unless we're in the correct state
        if ( appState != AppState::Polling )
            return;

        // Check if the world timer changed, this calls hasChanged if changed
        worldTimerMoniter.check();
    }

    // Constructor
    DllMain()
        : SpectatorManager ( &netMan, &procMan )
        , worldTimerMoniter ( this, Variable::WorldTime, *CC_WORLD_TIMER_ADDR )
    {
        // Timer and controller initialization is not done here because of threading issues

        procMan.connectPipe();

        netplayStateChanged ( NetplayState::PreInitial );

        ChangeMonitor::get().addRef ( this, Variable ( Variable::GameMode ), *CC_GAME_MODE_ADDR );
        ChangeMonitor::get().addRef ( this, Variable ( Variable::RoundStart ), AsmHacks::roundStartCounter );
        ChangeMonitor::get().addRef ( this, Variable ( Variable::SkippableFlag ), *CC_SKIPPABLE_FLAG_ADDR );

#ifndef RELEASE
        ChangeMonitor::get().addRef ( this, Variable ( Variable::MenuConfirmState ), AsmHacks::menuConfirmState );
        // ChangeMonitor::get().addRef ( this, Variable ( Variable::GameStateCounter ), *CC_GAME_STATE_COUNTER_ADDR );
        ChangeMonitor::get().addRef ( this, Variable ( Variable::CurrentMenuIndex ), AsmHacks::currentMenuIndex );
        ChangeMonitor::get().addPtrToRef ( this, Variable ( Variable::AutoReplaySave ),
                                           const_cast<const uint32_t *&> ( AsmHacks::autoReplaySaveStatePtr ), 0u );
#endif
    }

    // Destructor
    ~DllMain()
    {
        syncLog.deinitialize();

        procMan.disconnectPipe();

        ControllerManager::get().owner = 0;

        // Timer and controller deinitialization is not done here because of threading issues
    }

private:

    bool wasLastRoundDoubleGamePointDraw = false;

    void updateGameOverFlags()
    {
        const bool isKnockOut = ( ( *CC_P1_HEALTH_ADDR ) == 0 ) || ( ( *CC_P2_HEALTH_ADDR ) == 0 );
        const bool isTimeOut = ( ( *CC_ROUND_TIMER_ADDR ) == 0 );

        if ( !isKnockOut && !isTimeOut )
        {
            isGameOver = false;
        }
        else
        {
            const bool isP1GamePoint = ( ( *CC_P1_WINS_ADDR ) + 1 == ( *CC_WIN_COUNT_VS_ADDR ) );
            const bool isP2GamePoint = ( ( *CC_P2_WINS_ADDR ) + 1 == ( *CC_WIN_COUNT_VS_ADDR ) );
            const bool didP1Win = ( ( *CC_P1_HEALTH_ADDR ) > ( *CC_P2_HEALTH_ADDR ) );
            const bool didP2Win = ( ( *CC_P1_HEALTH_ADDR ) < ( *CC_P2_HEALTH_ADDR ) );
            const bool isDraw = ( ( *CC_P1_HEALTH_ADDR ) == ( *CC_P2_HEALTH_ADDR ) );

            if ( ( isP1GamePoint && didP1Win ) || ( isP2GamePoint && didP2Win ) )
                isGameOver = true;
            else if ( isP1GamePoint && isP2GamePoint && isDraw && wasLastRoundDoubleGamePointDraw )
                isGameOver = true;
            else
                isGameOver = false;

            LOG ( "[%s] p1wins=%u; p2wins=%u; p1health=%u; p2health=%u; wasLastRoundDoubleGamePointDraw=%u",
                  netMan.getIndexedFrame(), *CC_P1_WINS_ADDR, *CC_P2_WINS_ADDR, *CC_P1_HEALTH_ADDR, *CC_P2_HEALTH_ADDR,
                  wasLastRoundDoubleGamePointDraw );

            wasLastRoundDoubleGamePointDraw = ( isP1GamePoint && isP2GamePoint && isDraw );
        }

        LOG ( "[%s] isGameOver=%u", netMan.getIndexedFrame(), ( isGameOver ? 1 : 0 ) );
    }

    void saveMappings ( const Controller *controller ) const
    {
        if ( !controller )
            return;

        const string file = options.arg ( Options::AppDir ) + FOLDER + controller->getName() + MAPPINGS_EXT;

        LOG ( "Saving: %s", file );

        if ( controller->saveMappings ( file ) )
            return;

        LOG ( "Failed to save: %s", file );
    }

    // Filter simultaneous up / down and left / right directions.
    // Prioritize down and left for keyboard only.
    static uint16_t filterSimulDirState ( uint16_t state, bool isKeyboard )
    {
        if ( isKeyboard )
        {
            if ( ( state & ( BIT_UP | BIT_DOWN ) ) == ( BIT_UP | BIT_DOWN ) )
                state &= ~BIT_UP;
            if ( ( state & ( BIT_LEFT | BIT_RIGHT ) ) == ( BIT_LEFT | BIT_RIGHT ) )
                state &= ~BIT_RIGHT;
        }
        else
        {
            if ( ( state & ( BIT_UP | BIT_DOWN ) ) == ( BIT_UP | BIT_DOWN ) )
                state &= ~ ( BIT_UP | BIT_DOWN );
            if ( ( state & ( BIT_LEFT | BIT_RIGHT ) ) == ( BIT_LEFT | BIT_RIGHT ) )
                state &= ~ ( BIT_LEFT | BIT_RIGHT );
        }

        return state;
    }

    static uint16_t convertInputState ( uint32_t state, bool isKeyboard )
    {
        const uint16_t dirs = filterSimulDirState ( state & MASK_DIRS, isKeyboard );
        const uint16_t buttons = ( state & MASK_BUTTONS ) >> 8;

        uint8_t direction = 5;

        if ( dirs & BIT_UP )
            direction = 8;
        else if ( dirs & BIT_DOWN )
            direction = 2;

        if ( dirs & BIT_LEFT )
            --direction;
        else if ( dirs & BIT_RIGHT )
            ++direction;

        if ( direction == 5 )
            direction = 0;

        return COMBINE_INPUT ( direction, buttons );
    }

    static uint16_t getPrevInput ( const Controller *controller )
    {
        if ( !controller )
            return 0;

        return convertInputState ( controller->getPrevState(), controller->isKeyboard() );
    }

    static uint16_t getInput ( const Controller *controller )
    {
        if ( !controller )
            return 0;

        return convertInputState ( controller->getState(), controller->isKeyboard() );
    }

    static bool isButtonPressed ( const Controller *controller, uint16_t button )
    {
        if ( !controller )
            return false;

        button = COMBINE_INPUT ( 0, button );
        return ( getInput ( controller ) & button ) && ! ( getPrevInput ( controller ) & button );
    }

    static bool isDirectionPressed ( const Controller *controller, uint16_t dir )
    {
        if ( !controller )
            return false;

        return ( ( getInput ( controller ) & MASK_DIRS ) == dir )
               && ( ( getPrevInput ( controller ) & MASK_DIRS ) != dir );
    }

    static bool isAnyButtonReleased ( const Controller *controller )
    {
        if ( !controller )
            return false;

        return ( !controller->getAnyButton() && controller->getPrevAnyButton() );
    }
};


static void initializeDllMain()
{
    main.reset ( new DllMain() );
}

static void deinitialize()
{
    LOCK ( deinitMutex );

    if ( appState == AppState::Deinitialized )
        return;

    main.reset();

    EventManager::get().release();
    TimerManager::get().deinitialize();
    SocketManager::get().deinitialize();
    // Joystick must be deinitialized on the same thread it was initialized, ie not here
    Logger::get().deinitialize();

    DllHacks::deinitialize();

    appState = AppState::Deinitialized;
}

extern "C" BOOL APIENTRY DllMain ( HMODULE, DWORD reason, LPVOID )
{
    switch ( reason )
    {
        case DLL_PROCESS_ATTACH:
        {
            char buffer[4096];
            string gameDir;

            if ( GetModuleFileName ( 0, buffer, sizeof ( buffer ) ) )
            {
                gameDir = buffer;
                gameDir = gameDir.substr ( 0, gameDir.find_last_of ( "/\\" ) );

                replace ( gameDir.begin(), gameDir.end(), '/', '\\' );

                if ( !gameDir.empty() && gameDir.back() != '\\' )
                    gameDir += '\\';
            }

            ProcessManager::gameDir = gameDir;

            srand ( time ( 0 ) );

            Logger::get().initialize ( gameDir + LOG_FILE );
            LOG ( "DLL_PROCESS_ATTACH" );

            // We want the DLL to be able to rebind any previously bound ports
            Socket::forceReusePort ( true );

            try
            {
                // It is safe to initialize sockets here
                SocketManager::get().initialize();
                DllHacks::initializePreLoad();
                initializeDllMain();
            }
            catch ( const Exception& exc )
            {
                exit ( -1 );
            }
#ifdef NDEBUG
            catch ( const std::exception& exc )
            {
                exit ( -1 );
            }
            catch ( ... )
            {
                exit ( -1 );
            }
#endif
            break;
        }

        case DLL_PROCESS_DETACH:
            LOG ( "DLL_PROCESS_DETACH" );
            appState = AppState::Stopping;
            EventManager::get().release();
            exit ( 0 );
            break;
    }

    return TRUE;
}


static void stopDllMain ( const string& error )
{
    if ( main )
    {
        main->delayedStop ( error );
    }
    else
    {
        appState = AppState::Stopping;
        EventManager::get().stop();
    }
}

namespace AsmHacks
{

extern "C" void callback()
{
    if ( appState == AppState::Deinitialized )
        return;

    try
    {
        if ( appState == AppState::Uninitialized )
        {
            DllHacks::initializePostLoad();
            KeyboardState::windowHandle = DllHacks::windowHandle;

            // Joystick and timer must be initialized in the main thread
            TimerManager::get().initialize();
            ControllerManager::get().initialize ( 0 );

            // Start polling now
            EventManager::get().startPolling();
            appState = AppState::Polling;
        }

        ASSERT ( main.get() != 0 );

        main->callback();
    }
    catch ( const Exception& exc )
    {
        LOG ( "Stopping due to exception: %s", exc );
        stopDllMain ( exc.user );
    }
#ifdef NDEBUG
    catch ( const std::exception& exc )
    {
        LOG ( "Stopping due to std::exception: %s", exc.what() );
        stopDllMain ( string ( "Error: " ) + exc.what() );
    }
    catch ( ... )
    {
        LOG ( "Stopping due to unknown exception!" );
        stopDllMain ( "Unknown error!" );
    }
#endif

    if ( appState == AppState::Stopping )
    {
        LOG ( "Exiting" );

        // Joystick must be deinitialized on the main thread it was initialized
        ControllerManager::get().deinitialize();
        deinitialize();
        exit ( 0 );
    }
}

} // namespace AsmHacks


class PollThread : Thread
{
    bool stalled = false;

public:

    ~PollThread() { join(); }

    void start()
    {
        stalled = true;
        Thread::start();
    }

    void join()
    {
        stalled = false;
        Thread::join();
    }

    void run() override
    {
        // Poll until we are not stalled
        while ( stalled )
        {
            if ( !EventManager::get().poll ( 100 ) )
            {
                appState = AppState::Stopping;
                return;
            }

            Sleep ( 1 );
        }
    }
};

static PollThread pollThread;

void startStall()
{
    if ( !main || appState != AppState::Polling )
        return;

    LOG ( "[%s] worldTimer=%u; Starting to poll while stalled", main->netMan.getIndexedFrame(), *CC_WORLD_TIMER_ADDR );

    pollThread.start();
}

void stopStall()
{
    if ( !main )
        return;

    pollThread.join();

    LOG ( "[%s] worldTimer=%u; Stopped polling while stalled", main->netMan.getIndexedFrame(), *CC_WORLD_TIMER_ADDR );
}
