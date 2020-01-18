Import("env")

# access to global construction environment
#print env

build_tag = env['PIOENV']
env.Replace(PROGNAME="%s_firmware" % build_tag)

# Dump construction environments (for debug purpose)
#print env.Dump()