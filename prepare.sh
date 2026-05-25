#!/bin/sh

curl https://www.lua.org/ftp/lua-5.5.0.tar.gz -o lua-5.5.0.tar.gz
tar -zxvf lua-5.5.0.tar.gz
rm -rf skynet/3rd/lua
mv lua-5.5.0/src skynet/3rd/lua

