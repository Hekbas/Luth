#pragma once

#include "luth/core/App.h"
#include "luth/core/Log.h"

int main()
{
    Luth::Log::Init();
    Luth::App* app = Luth::CreateApp();
    app->Run();
    delete app;
}
