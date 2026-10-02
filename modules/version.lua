  
function main (target)
	local host = os.host()
	local subhost = os.subhost()

	local system
	if (host ~= subhost) then
		system = host .. "/" .. subhost
	else
		system = host
	end

	local branch = "unknown-branch"
	local commit = "unknown-commit"
	local timestamp = ""
	local describe = "unknown-version"
	local protocol = "unknown-protocol"
	try
	{
		function ()
			import("detect.tools.find_git")
			local git = find_git()
			if (git) then
				branch = os.iorunv(git, {"rev-parse", "--abbrev-ref", "HEAD"}):trim()
				commit = os.iorunv(git, {"rev-parse", "--short", "HEAD"}):trim()
				timestamp = os.iorunv(git, {"log", "-1", "--date=short", "--pretty=format:%ci"}):trim()
				-- The version the client and server compare on connect. An uncommitted tree adds "-dirty."
				-- and a hash of what is uncommitted, so any change to the tree changes the version, and two
				-- different binaries can never claim the same one (on 2026-09-18 a client and a server built
				-- from different trees both said "-dirty" and passed the check with different protocols).
				describe = os.iorunv(git, {"describe", "--tags", "--always"}):trim()
				-- Submodule state is left out: Libraries/TiltedReverse carries a local fix whose upstream we cannot
				-- push to, and nothing the server links comes from it, so it must not keep every build "dirty".
				--
				-- Code/bot is left out too. The test bot links the same encoding library, so it cannot change the
				-- protocol, but until 2026-09-24 editing it changed this hash and therefore the version the client
				-- and server compare: every bot change locked the bot out of the running server and needed the
				-- server, and the player's client, rebuilt to match. That made automated testing impossible in
				-- practice. A bot-only change is now invisible here, which is the truth of it.
				-- Documentation and notes are excluded for the same reason as Code/bot: a markdown file cannot change
				-- the protocol, and on 2026-09-25 editing VR_TODO.md moved the version and locked the test bot out of
				-- the running server, which then reported a pass because it never connected at all.
				local exclude = {":(exclude)Code/bot", ":(exclude)*.md", ":(exclude)Tools/VR/*.py", ":(exclude)Tools/VR/*.ps1"}
				local diffArgs = {"diff", "HEAD", "--ignore-submodules", "--"}
				local statusArgs = {"status", "--porcelain", "--ignore-submodules", "--"}
				table.insert(diffArgs, ".")
				table.insert(statusArgs, ".")
				for _, pattern in ipairs(exclude) do
					table.insert(diffArgs, pattern)
					table.insert(statusArgs, pattern)
				end
				local uncommitted = os.iorunv(git, diffArgs) .. os.iorunv(git, statusArgs)
				if uncommitted:trim() ~= "" then
					local tmp = os.tmpfile()
					io.writefile(tmp, uncommitted)
					describe = describe .. "-dirty." .. os.iorunv(git, {"hash-object", tmp}):trim():sub(1, 7)
					os.rm(tmp)
				end

				-- The protocol id: what the server actually compares on connect. It is a digest of the contents of
				-- Code/encoding -- every message, struct and opcode -- and of nothing else, so two builds accept
				-- each other exactly when they agree on what goes over the wire. The version above changes with any
				-- edit anywhere and with every commit, which is right for telling builds apart and wrong for
				-- deciding who may talk to whom: on 2026-10-01 a build made before a commit and one made after it,
				-- from the same files, could not connect to each other.
				--
				-- Contents, not history: each file is hashed by git (which also evens out line endings between a
				-- Windows and a Linux checkout), and the list of path-and-hash pairs is folded in order. Committed,
				-- staged or neither makes no difference.
				local listed = os.iorunv(git, {"ls-files", "-co", "--exclude-standard", "--", "Code/encoding"})
				local paths = {}
				for line in listed:gmatch("[^\r\n]+") do
					if os.isfile(line) then
						table.insert(paths, line)
					end
				end
				table.sort(paths)
				local folded = 2166136261
				local function fold(text)
					for i = 1, #text do
						-- FNV-1a in plain arithmetic: xor the low byte, then multiply by 16777619 modulo 2^32.
						local low = folded % 256
						local byte = text:byte(i)
						local mixed = 0
						local bit = 1
						for _ = 1, 8 do
							if (low % 2) ~= (byte % 2) then
								mixed = mixed + bit
							end
							low = math.floor(low / 2)
							byte = math.floor(byte / 2)
							bit = bit * 2
						end
						folded = folded - (folded % 256) + mixed
						folded = (((folded % 256) * 16777216) + folded * 403) % 4294967296
					end
				end
				local batch = 60
				for first = 1, #paths, batch do
					local args = {"hash-object", "--"}
					for i = first, math.min(first + batch - 1, #paths) do
						table.insert(args, paths[i])
					end
					local hashes = os.iorunv(git, args)
					local i = first
					for hash in hashes:gmatch("[^\r\n]+") do
						fold(paths[i] .. ":" .. hash:trim() .. ";")
						i = i + 1
					end
				end
				if #paths > 0 then
					protocol = string.format("%08x", folded)
				end
			else
				error("git not found")
			end
		end,

		catch
		{
			function (err)
				print(string.format("Failed to retrieve git data: %s", err))
			end
		}
    }

    return branch, commit, timestamp, describe, protocol
end