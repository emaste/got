REGRESS_TARGETS=checkout update status log add rm diff blame branch tag \
	ref commit revert cherrypick backout rebase import histedit \
	integrate stage unstage cat clone fetch tree
NOOBJ=Yes

GOT_TEST_ROOT=/tmp

checkout:
	${.CURDIR}/checkout.sh -q -r "$(GOT_TEST_ROOT)"

update:
	${.CURDIR}/update.sh -q -r "$(GOT_TEST_ROOT)"

status:
	${.CURDIR}/status.sh -q -r "$(GOT_TEST_ROOT)"

log:
	${.CURDIR}/log.sh -q -r "$(GOT_TEST_ROOT)"

add:
	${.CURDIR}/add.sh -q -r "$(GOT_TEST_ROOT)"

rm:
	${.CURDIR}/rm.sh -q -r "$(GOT_TEST_ROOT)"

diff:
	${.CURDIR}/diff.sh -q -r "$(GOT_TEST_ROOT)"

blame:
	${.CURDIR}/blame.sh -q -r "$(GOT_TEST_ROOT)"

branch:
	${.CURDIR}/branch.sh -q -r "$(GOT_TEST_ROOT)"

tag:
	${.CURDIR}/tag.sh -q -r "$(GOT_TEST_ROOT)"

ref:
	${.CURDIR}/ref.sh -q -r "$(GOT_TEST_ROOT)"

commit:
	${.CURDIR}/commit.sh -q -r "$(GOT_TEST_ROOT)"

revert:
	${.CURDIR}/revert.sh -q -r "$(GOT_TEST_ROOT)"

cherrypick:
	${.CURDIR}/cherrypick.sh -q -r "$(GOT_TEST_ROOT)"

backout:
	${.CURDIR}/backout.sh -q -r "$(GOT_TEST_ROOT)"

rebase:
	${.CURDIR}/rebase.sh -q -r "$(GOT_TEST_ROOT)"

import:
	${.CURDIR}/import.sh -q -r "$(GOT_TEST_ROOT)"

histedit:
	${.CURDIR}/histedit.sh -q -r "$(GOT_TEST_ROOT)"

integrate:
	${.CURDIR}/integrate.sh -q -r "$(GOT_TEST_ROOT)"

stage:
	${.CURDIR}/stage.sh -q -r "$(GOT_TEST_ROOT)"

unstage:
	${.CURDIR}/unstage.sh -q -r "$(GOT_TEST_ROOT)"

cat:
	${.CURDIR}/cat.sh -q -r "$(GOT_TEST_ROOT)"

clone:
	${.CURDIR}/clone.sh -q -r "$(GOT_TEST_ROOT)"

fetch:
	${.CURDIR}/fetch.sh -q -r "$(GOT_TEST_ROOT)"

tree:
	${.CURDIR}/tree.sh -q -r "$(GOT_TEST_ROOT)"

regress: ${REGRESS_TARGETS} .PHONY .SILENT

.include <bsd.prog.mk>
