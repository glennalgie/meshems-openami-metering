from pathlib import Path

Import("env")

framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
project_include = Path(env["PROJECT_INCLUDE_DIR"])
include_paths = []

if project_include.is_dir():
    include_paths.append(str(project_include))

if framework_dir:
    for library in ("WiFi", "Network"):
        src = Path(framework_dir) / "libraries" / library / "src"
        if src.is_dir():
            include_paths.append(str(src))

if include_paths:
    env.Append(CPPPATH=include_paths)
    print("add_arduino_network_include: CPPPATH += %s" % ", ".join(include_paths))
