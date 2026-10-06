extends "res://tests/test_case.gd"
## Resolving conflicts: a conflicted file read as blocks (what both sides agree on, and each
## conflict's mine / base / theirs), checked against git merge-file; resolving with your own text
## or a whole side; "mine" staying yours during a rebase; deletions, binary files, CRLF.


func run() -> void:
	_blocks_match_git()
	_resolve_with_text()
	_resolve_with_side()
	_rebase_mine_is_yours()
	_deleted_on_one_side()
	_binary()
	_crlf()
	_import_settings()


const BASE := "one\ntwo\nthree\nfour\nfive\nsix\nseven\n"


## main and feature both change "four" (a conflict); feature also changes "one" and main "seven"
## (merged on their own). Merging feature into main stops at the conflict.
func _merge_conflict(name: String) -> String:
	var repo := make_repo(name)
	write(repo.path_join("a.txt"), BASE)
	commit_all(repo, "first")
	git(repo, ["checkout", "-q", "-b", "feature"])
	write(repo.path_join("a.txt"), BASE.replace("one", "ONE").replace("four", "four (feature)"))
	commit_all(repo, "Feature change")
	git(repo, ["checkout", "-q", "main"])
	write(repo.path_join("a.txt"), BASE.replace("seven", "SEVEN").replace("four", "four (main)"))
	commit_all(repo, "Main change")
	git(repo, ["merge", "feature"])
	return repo


## The file with each conflict replaced by p_side's text ("mine", "theirs", or "both": mine then
## theirs).
func _joined(conflict: Dictionary, side: String) -> String:
	var text := ""
	for block in conflict.blocks:
		if block.kind == "same":
			text += block.text
		elif side == "both":
			text += block.mine + block.theirs
		else:
			text += block[side]
	return text


## What git merge-file makes of the index's three versions, favoring p_flag's side.
func _git_merge_file(repo: String, path: String, flag: String) -> String:
	for stage in [1, 2, 3]:
		write(repo.path_join(".git/stage%d" % stage), git(repo, ["show", ":%d:%s" % [stage, path]]) + "\n")
	return git(repo, ["merge-file", "-p", flag, ".git/stage2", ".git/stage1", ".git/stage3"]) + "\n"


func _blocks_match_git() -> void:
	var repo := _merge_conflict("blocks")
	var r := open(repo)
	var conflict := r.get_conflict("a.txt")
	var kinds := []
	for block in conflict.blocks:
		kinds.append(block.kind)
	check("blocks: same, conflict, same", kinds == ["same", "conflict", "same"], kinds)
	check("blocks: the conflict's sides", conflict.blocks[1].mine == "four (main)\n" and conflict.blocks[1].theirs == "four (feature)\n" and conflict.blocks[1].base == "four\n", conflict.blocks[1])
	check("blocks: changes on one side are already in", conflict.blocks[0].text.begins_with("ONE\n") and conflict.blocks[2].text.ends_with("SEVEN\n"), conflict.blocks)
	check("blocks: taking mine everywhere is git merge-file --ours", _joined(conflict, "mine") == _git_merge_file(repo, "a.txt", "--ours"), _joined(conflict, "mine"))
	check("blocks: taking theirs everywhere is git merge-file --theirs", _joined(conflict, "theirs") == _git_merge_file(repo, "a.txt", "--theirs"))
	check("blocks: taking both is git merge-file --union", _joined(conflict, "both") == _git_merge_file(repo, "a.txt", "--union"))
	check("blocks: the sides named after their branches", conflict.mine_label == "main" and conflict.theirs_label == "feature", [conflict.mine_label, conflict.theirs_label])
	check("blocks: a file without a conflict has none", r.get_conflict("nothing.txt").is_empty())


func _resolve_with_text() -> void:
	var repo := _merge_conflict("resolve-text")
	var r := open(repo)
	var text := _joined(r.get_conflict("a.txt"), "both")
	check("resolve: with text", r.resolve_conflict("a.txt", text) == OK, GitRepository.get_last_error())
	check("resolve: the file has it", read(repo.path_join("a.txt")) == text, read(repo.path_join("a.txt")))
	check("resolve: no longer conflicted, staged", r.get_operation().conflicts.is_empty() and git(repo, ["diff", "--cached", "--name-only"]) == "a.txt", git(repo, ["status", "--porcelain"]))
	check("resolve: then the merge commits", r.continue_operation() == OK and git(repo, ["show", "HEAD:a.txt"]) + "\n" == text, GitRepository.get_last_error())
	check("resolve: nothing left to resolve", r.resolve_conflict("a.txt", "x\n") != OK)


func _resolve_with_side() -> void:
	var repo := _merge_conflict("resolve-side")
	var r := open(repo)
	var theirs := git(repo, ["show", ":3:a.txt"]) + "\n"
	check("take theirs", r.resolve_conflict_with("a.txt", "theirs") == OK, GitRepository.get_last_error())
	check("take theirs: the whole file is theirs", read(repo.path_join("a.txt")) == theirs, read(repo.path_join("a.txt")))
	check("take theirs: resolved", r.get_operation().conflicts.is_empty())


## In a rebase git calls the branch being rebased onto "ours"; mine must still be your commit.
func _rebase_mine_is_yours() -> void:
	var repo := make_repo("rebase")
	write(repo.path_join("a.txt"), "one\n")
	commit_all(repo, "first")
	git(repo, ["checkout", "-q", "-b", "feature"])
	write(repo.path_join("a.txt"), "my change\n")
	commit_all(repo, "Mine")
	git(repo, ["checkout", "-q", "main"])
	write(repo.path_join("a.txt"), "upstream change\n")
	commit_all(repo, "Upstream")
	git(repo, ["checkout", "-q", "feature"])
	git(repo, ["rebase", "main"])
	var conflict := open(repo).get_conflict("a.txt")
	check("rebase: labels: your branch, and the one it's rebased onto", conflict.mine_label == "feature" and conflict.theirs_label == "main", [conflict.mine_label, conflict.theirs_label])
	check("rebase: mine is your commit", conflict.kind == "rebase" and conflict.blocks.size() == 1 and conflict.blocks[0].mine == "my change\n" and conflict.blocks[0].theirs == "upstream change\n", conflict)
	check("rebase: taking mine keeps your change", open(repo).resolve_conflict_with("a.txt", "mine") == OK and read(repo.path_join("a.txt")) == "my change\n", read(repo.path_join("a.txt")))


func _deleted_on_one_side() -> void:
	var repo := make_repo("deleted")
	write(repo.path_join("a.txt"), "one\n")
	commit_all(repo, "first")
	git(repo, ["checkout", "-q", "-b", "feature"])
	git(repo, ["rm", "-q", "a.txt"])
	git(repo, ["commit", "-q", "-m", "Delete a"])
	git(repo, ["checkout", "-q", "main"])
	write(repo.path_join("a.txt"), "changed\n")
	commit_all(repo, "Change a")
	git(repo, ["merge", "feature"])
	var r := open(repo)
	var conflict := r.get_conflict("a.txt")
	check("deleted: theirs doesn't exist, no blocks", conflict.mine_exists and not conflict.theirs_exists and conflict.blocks.is_empty(), conflict)
	check("deleted: taking theirs deletes it", r.resolve_conflict_with("a.txt", "theirs") == OK and not exists(repo.path_join("a.txt")), GitRepository.get_last_error())
	check("deleted: resolved, as git sees it", r.get_operation().conflicts.is_empty() and git(repo, ["diff", "--name-only", "--diff-filter=U"]) == "", git(repo, ["status", "--porcelain"]))


func _binary() -> void:
	var repo := make_repo("binary")
	var data := PackedByteArray([0, 1, 2, 3, 0, 255])
	var f := FileAccess.open(repo.path_join("b.bin"), FileAccess.WRITE)
	f.store_buffer(data)
	f.close()
	commit_all(repo, "first")
	git(repo, ["checkout", "-q", "-b", "feature"])
	f = FileAccess.open(repo.path_join("b.bin"), FileAccess.WRITE)
	f.store_buffer(PackedByteArray([0, 9, 9, 9, 0, 255]))
	f.close()
	commit_all(repo, "Feature bytes")
	git(repo, ["checkout", "-q", "main"])
	var mine := PackedByteArray([0, 7, 7, 7, 0, 255])
	f = FileAccess.open(repo.path_join("b.bin"), FileAccess.WRITE)
	f.store_buffer(mine)
	f.close()
	commit_all(repo, "Main bytes")
	git(repo, ["merge", "feature"])
	var r := open(repo)
	check("binary: seen as binary, no blocks", r.get_conflict("b.bin").binary and r.get_conflict("b.bin").blocks.is_empty(), r.get_conflict("b.bin"))
	check("binary: taking mine keeps my bytes", r.resolve_conflict_with("b.bin", "mine") == OK and FileAccess.get_file_as_bytes(repo.path_join("b.bin")) == mine)


## With core.autocrlf the file gets CRLF, as a checkout would write it.
func _crlf() -> void:
	var repo := _merge_conflict("crlf")
	git(repo, ["config", "core.autocrlf", "true"])
	var r := open(repo)
	check("crlf: resolve", r.resolve_conflict("a.txt", "a\nb\n") == OK, GitRepository.get_last_error())
	check("crlf: written with CRLF", FileAccess.get_file_as_string(repo.path_join("a.txt")) == "a\r\nb\r\n", FileAccess.get_file_as_string(repo.path_join("a.txt")).c_escape())
	check("crlf: staged as LF", git(repo, ["show", ":a.txt"]) == "a\nb", git(repo, ["show", ":a.txt"]))


const IMPORT_BASE := "[remap]\n\nimporter=\"texture\"\ntype=\"CompressedTexture2D\"\n\n[params]\n\ncompress/mode=0\ncompress/lossy_quality=0.7\nmipmaps/generate=false\nroughness/mode=0\n"


## An .import file: main sets compress/mode to 2 and the lossy quality, feature sets compress/mode
## to 1 and turns mipmaps on. The lines are next to each other, so git sees one conflict; by setting,
## only compress/mode is one.
func _import_settings() -> void:
	var repo := make_repo("import-settings")
	write(repo.path_join("icon.png.import"), IMPORT_BASE)
	commit_all(repo, "first")
	git(repo, ["checkout", "-q", "-b", "feature"])
	write(repo.path_join("icon.png.import"), IMPORT_BASE.replace("compress/mode=0", "compress/mode=1").replace("generate=false", "generate=true"))
	commit_all(repo, "Feature change")
	git(repo, ["checkout", "-q", "main"])
	write(repo.path_join("icon.png.import"), IMPORT_BASE.replace("compress/mode=0", "compress/mode=2").replace("quality=0.7", "quality=0.8"))
	commit_all(repo, "Main change")
	git(repo, ["merge", "feature"])
	var r := open(repo)
	var conflict := r.get_conflict("icon.png.import")
	check("settings: git sees a conflict", conflict.blocks.size() > 1, conflict)
	var ids := []
	for setting in conflict.get("settings", []):
		ids.append(setting.id)
	check("settings: only compress/mode is asked about", ids == ["params\ncompress/mode"], conflict.get("settings"))
	check("settings: its three values", conflict.settings[0].base == "0" and conflict.settings[0].mine == "2" and conflict.settings[0].theirs == "1", conflict.settings)
	check("settings: the importer", conflict.importer == "texture", conflict.importer)
	check("settings: refused without a choice", r.resolve_settings_conflict("icon.png.import", {}) != OK)
	check("settings: resolved with theirs", r.resolve_settings_conflict("icon.png.import", {"params\ncompress/mode": "theirs"}) == OK, GitRepository.get_last_error())
	var expected := IMPORT_BASE.replace("compress/mode=0", "compress/mode=1").replace("quality=0.7", "quality=0.8").replace("generate=false", "generate=true")
	check("settings: theirs' mode, mine's quality, theirs' mipmaps", read(repo.path_join("icon.png.import")) == expected, read(repo.path_join("icon.png.import")))
	check("settings: staged, no conflicts left", r.get_operation().conflicts.is_empty(), r.get_operation())
