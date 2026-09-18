  
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
				local uncommitted = os.iorunv(git, {"diff", "HEAD"}) .. os.iorunv(git, {"status", "--porcelain"})
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