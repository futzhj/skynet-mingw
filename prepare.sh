#!/bin/sh

curl https://www.lua.org/ftp/lua-5.5.0.tar.gz -o lua-5.5.0.tar.gz
tar -zxvf lua-5.5.0.tar.gz
rm -rf skynet/3rd/lua
mv lua-5.5.0 skynet/3rd/lua
ln -s -f skynet/3rd/ 3rd
ln -s skynet/examples/ examples 
ln -s skynet/lualib/ lualib  
ln -s skynet/lualib-src/ lualib-src  
ln -s skynet/service/ service 
ln -s skynet/service-src/ service-src  
ln -s skynet/skynet-src/ skynet-src 
ln -s skynet/test/ test
