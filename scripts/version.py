# Define FW_VERSION (hash de git + fecha de compilación) solo para el código del proyecto,
# así se puede ver en el panel qué versión corre cada equipo.
import subprocess
from datetime import datetime


try:
    rev = subprocess.check_output(["git", "describe", "--always", "--dirty"], text=True).strip()
except Exception:
    rev = "sin-git"

version = "%s %s" % (rev, datetime.now().strftime("%Y-%m-%d %H:%M"))
print("FW_VERSION = " + version)

try:
    Import("projenv")
except Exception:
    projenv = None  # Sin compilación (ej. -t nobuild): no hace falta definir nada
if projenv is not None:
    projenv.Append(CPPDEFINES=[("FW_VERSION", '\\"%s\\"' % version)])
