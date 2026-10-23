## 機能

ほぼほぼScoreTrackerです

bg.pngにはRLの魂が宿っています（！？）

ScoreTrackerとの違いとしては、機能的にはF3の設定機能がなくなって、対戦中のみの表示になった代わりに

接続を切っても勝敗数が引き継がれるといったところです

天則観のデータベースを読み込んで判定しているので、天則観を導入していないと動きません

SWRSToys.iniにはこれを追記してください

```
ScoreTransfer=Modules/ScoreTransfer/ScoreTransfer.dll
```

20261023
