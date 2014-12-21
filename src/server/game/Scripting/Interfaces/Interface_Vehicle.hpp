/// Copyright Ashran 2014-2015

#ifndef SCRIPTING_INTERFACES_VEHICLE
#define SCRIPTING_INTERFACES_VEHICLE

#include "InterfaceBase.hpp"

namespace MS { namespace Game { namespace Scripting { namespace Interfaces 
{

    /// Vehicle Script Interface
    class VehicleScript : public ScriptObjectImpl<true>
    {
        protected:
            /// Constructor
            /// @p_Name : Script Name
            VehicleScript(const char * p_Name);

        public:
            /// Called after a vehicle is installed.
            /// @p_Vehicle : Vehicle instance
            virtual void OnInstall(Vehicle* p_Vehicle)
            { 
                UNUSED(p_Vehicle); 
            }
            /// Called after a vehicle is uninstalled.
            /// @p_Vehicle : Vehicle instance
            virtual void OnUninstall(Vehicle* p_Vehicle)
            {
                UNUSED(p_Vehicle);
            }

            /// Called when a vehicle resets.
            /// @p_Vehicle : Vehicle instance
            virtual void OnReset(Vehicle* p_Vehicle)
            {
                UNUSED(p_Vehicle);
            }

            /// Called after an accessory is installed in a vehicle.
            /// @p_Vehicle   : Vehicle instance
            /// @p_Accessory : Accessory to install
            virtual void OnInstallAccessory(Vehicle * p_Vehicle, Creature * p_Accessory)
            {
                UNUSED(p_Vehicle);
            }

            /// Called after a passenger is added to a vehicle.
            /// @p_Vehicle   : Vehicle instance
            /// @p_Passanger : Passenger to add
            /// @p_SeatID    : Passenger destination seat ID
            virtual void OnAddPassenger(Vehicle * p_Vehicle, Unit * p_Passenger, int8 p_SeatID)
            {
                UNUSED(p_Vehicle);
                UNUSED(p_Passenger);
                UNUSED(p_SeatID);
            }
            /// Called after a passenger is removed from a vehicle.
            /// @p_Vehicle   : Vehicle instance
            /// @p_Passanger : Passenger to remove
            virtual void OnRemovePassenger(Vehicle* p_Vehicle, Unit * p_Passenger)
            {
                UNUSED(p_Vehicle);
                UNUSED(p_Passenger);
            }

            /// Called when a CreatureAI object is needed for the creature.
            /// @p_Creature : target creature
            virtual CreatureAI* GetAI(Creature * p_Creature) const
            {
                return nullptr;
            }

    };

}   ///< Namespace Interfaces
}   ///< Namespace Scripting
}   ///< Namespace Game
}   ///< Namespace MS

#endif  ///< SCRIPTING_INTERFACES_VEHICLE