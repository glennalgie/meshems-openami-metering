Import("env")

from os.path import join


framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")

if framework_dir:
    env.Append(
        CPPPATH=[
            join(framework_dir, "libraries", "Network", "src"),
            join(framework_dir, "libraries", "WiFi", "src"),
        ]
    )
