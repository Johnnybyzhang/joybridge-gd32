"""Test the receiver through Steam's installed SDL3 Nintendo HIDAPI driver."""
import argparse
import ctypes as c
import json
from pathlib import Path
import time


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dll",type=Path,default=Path("C:/Program Files (x86)/Steam/SDL3.dll"))
    p.add_argument("--out",type=Path)
    args=p.parse_args()
    sdl=c.CDLL(str(args.dll))
    def api(name,args,result):
        fn=getattr(sdl,name); fn.argtypes=args; fn.restype=result; return fn
    hint=api("SDL_SetHint",[c.c_char_p,c.c_char_p],c.c_bool)
    init=api("SDL_Init",[c.c_uint32],c.c_bool)
    error=api("SDL_GetError",[],c.c_char_p)
    ids=api("SDL_GetGamepads",[c.POINTER(c.c_int)],c.POINTER(c.c_uint32))
    vendor=api("SDL_GetGamepadVendorForID",[c.c_uint32],c.c_uint16)
    product=api("SDL_GetGamepadProductForID",[c.c_uint32],c.c_uint16)
    name=api("SDL_GetGamepadNameForID",[c.c_uint32],c.c_char_p)
    open_pad=api("SDL_OpenGamepad",[c.c_uint32],c.c_void_p)
    rumble=api("SDL_RumbleGamepad",[c.c_void_p,c.c_uint16,c.c_uint16,c.c_uint32],c.c_bool)
    update=api("SDL_UpdateJoysticks",[],None)
    close=api("SDL_CloseGamepad",[c.c_void_p],None)
    free=api("SDL_free",[c.c_void_p],None)
    quit_sdl=api("SDL_Quit",[],None)
    hint(b"SDL_JOYSTICK_HIDAPI",b"1")
    hint(b"SDL_JOYSTICK_HIDAPI_SWITCH",b"1")
    hint(b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS",b"1")
    if not init(0x2000): raise RuntimeError(error().decode())
    count=c.c_int(); found=ids(c.byref(count)); pad=None
    result={"dll":str(args.dll),"targets":[],"rumble_ok":False}
    try:
        for i in range(count.value):
            ident=found[i]
            if vendor(ident)==0x057e and product(ident)==0x2009:
                result["targets"].append({"id":ident,"name":name(ident).decode()})
        if len(result["targets"])!=1: raise RuntimeError("Expected exactly one 057e:2009 gamepad")
        pad=open_pad(result["targets"][0]["id"])
        if not pad: raise RuntimeError(error().decode())
        result["rumble_ok"]=rumble(pad,0x6000,0x6000,1000)
        result["error"]=error().decode()
        until=time.monotonic()+1.5
        while time.monotonic()<until: update(); time.sleep(.01)
        result["stop_ok"]=rumble(pad,0,0,0)
        update(); time.sleep(.1)
    finally:
        if pad: rumble(pad,0,0,0); update(); close(pad)
        if found: free(found)
        quit_sdl()
        if args.out: args.out.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result,indent=2))


if __name__=="__main__": main()
