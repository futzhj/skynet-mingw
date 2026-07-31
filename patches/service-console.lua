local skynet = require "skynet"
local snax   = require "skynet.snax"
local socket = require "skynet.socket"

local function split_cmdline(cmdline)
	local split = {}
	for i in string.gmatch(cmdline, "%S+") do
		table.insert(split, i)
	end
	return split
end

local function console_main_loop()
	-- GUI and hidden Windows launches do not provide a readable stdin.
	-- Keep the optional local console from turning that into a service error.
	pcall(function()
		local stdin = socket.stdin()
		while true do
			local cmdline = socket.readline(stdin, "\n")
			if not cmdline then
				return
			end
			local split = split_cmdline(cmdline)
			local command = split[1]
			if command == "snax" then
				pcall(snax.newservice, select(2, table.unpack(split)))
			elseif cmdline ~= "" then
				pcall(skynet.newservice, cmdline)
			end
		end
	end)
end

skynet.start(function()
	skynet.fork(console_main_loop)
end)
