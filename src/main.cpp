#include "kernel.h"
#include <circle/startup.h>

int main(void)
{
    CKernel Kernel;
    if (!Kernel.Initialize()) {
        return EXIT_HALT;
    }

    TShutdownMode ShutdownMode = Kernel.Run();

    return ShutdownMode;
}
