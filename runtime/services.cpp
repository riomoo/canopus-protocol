// The registry of implemented services. Anything else the game opens is a
// logging stub (ipc.cpp).
#include "ipc.h"

namespace rt {
namespace ipc {

void RegisterApt();
void RegisterFs();
void RegisterMisc();
void RegisterGsp();
void RegisterDsp();
void RegisterY2r();

void RegisterAll()
{
    RegisterApt();
    RegisterFs();
    RegisterMisc();
    RegisterGsp();
    RegisterDsp();
    RegisterY2r();
}

}  // namespace ipc
}  // namespace rt
