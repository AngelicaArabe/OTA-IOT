Import("env")
from SCons.Script import DefaultEnvironment

def before_upload(source, target, env):
    print("=== Uploading SPIFFS before firmware ===")
    spiffs_env = DefaultEnvironment()
    spiffs_env.Replace(UPLOAD_PORT=env.get("UPLOAD_PORT"))
    spiffs_env.Replace(UPLOAD_SPEED=env.get("UPLOAD_SPEED"))
    spiffs_env.Execute("platformio run -t uploadfs")

env.AddPreAction("upload", before_upload)
