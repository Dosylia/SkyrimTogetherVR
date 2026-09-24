  
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
				local exclude = {":(exclude)Code/bot"}
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

    return branch, commit, timestamp, describe
end