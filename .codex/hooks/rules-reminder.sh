#!/bin/sh
# UserPromptSubmit: 毎回の依頼の前に、断定のルールを Claude の文脈に入れる (CLAUDE.md の要約)。
cat <<'TXT'
[このリポジトリのルール (CLAUDE.md)]
- 事実として書くのは、この会話で一次情報 (前後ごと読んだコードの行、ログの行、測った値) を見たことだけ。grep の当たりやファイル名では断定しない。見ていないことは「推測」「未確認」と書き、何を見れば確かめられるかを添える。
- 効いた/効かなかったは、変えたもの自体の測定で、分かっている揺れを考えて決める。足りなければ「まだ判断できない」。
- ゴーグルへの操作の前に、その前提を確かめ直す。再起動は電源ボタン (reboot は戻らない)。
- 誤りが分かったら memory/feedback-verify-before-asserting.md の Mistake log に足し、メモリは description と MEMORY.md の行も直す。
TXT
exit 0
