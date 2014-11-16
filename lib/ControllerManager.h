#pragma once

#include "Controller.h"

#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>


struct ControllerMappings : public SerializableSequence
{
    std::unordered_map<IndexedGuid, MsgPtr> mappings;

    DECLARE_MESSAGE_BOILERPLATE ( ControllerMappings )
};


class ControllerManager
{
public:

    struct Owner
    {
        virtual void attachedJoystick ( Controller *controller ) = 0;
        virtual void detachedJoystick ( Controller *controller ) = 0;
    };

    Owner *owner = 0;

private:

    // Keyboard controller instance
    Controller keyboard;

    // Maps of joystick controller instances
    std::unordered_map<int, std::shared_ptr<Controller>> joysticks;
    std::unordered_map<IndexedGuid, Controller *> joysticksByGuid;

    // Set of unique joystick guids, used to workaround SDL bug 2643
    std::unordered_set<Guid> guids;

    // Flag to reinitialize joysticks, used to workaround SDL bug 2643
    bool shouldReset = false;

    // Flag to indicate if initialized
    bool initialized = false;

    // Private constructor, etc. for singleton class
    ControllerManager();
    ControllerManager ( const ControllerManager& );
    const ControllerManager& operator= ( const ControllerManager& );

    // Check for controller events
    void doCheck();

public:

    // Check for controller events
    void check();

    // Clear controllers
    void clear();

    // Get the keyboard controller
    Controller *getKeyboard() { return &keyboard; }
    const Controller *getKeyboard() const { return &keyboard; }

    // Get the list of joysticks sorted by name
    std::vector<Controller *> getJoysticks();
    std::vector<const Controller *> getJoysticks() const;

    // Get all the controllers, sorted by name, except the keyboard is first
    std::vector<Controller *> getControllers();
    std::vector<const Controller *> getControllers() const;

    // Get the mappings for all controllers
    MsgPtr getMappings() const
    {
        ControllerMappings *msg = new ControllerMappings();

        for ( const Controller *c : getControllers() )
            msg->mappings[c->getGuid()] = c->getMappings();

        return MsgPtr ( msg );
    }

    // Set the mappings for all controllers
    void setMappings ( const ControllerMappings& mappings )
    {
        for ( auto& kv : mappings.mappings )
        {
            if ( kv.second->getMsgType() == MsgType::KeyboardMappings )
                keyboard.setMappings ( kv.second->getAs<KeyboardMappings>() );
            else if ( joysticksByGuid.find ( kv.first ) != joysticksByGuid.end() )
                joysticksByGuid[kv.first]->setMappings ( kv.second->getAs<JoystickMappings>() );
        }
    }

    // Initialize / deinitialize controller manager
    void initialize ( Owner *owner );
    void deinitialize();
    bool isInitialized() const { return initialized; }

    // Get the singleton instance
    static ControllerManager& get();
};
