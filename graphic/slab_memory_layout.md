https://dreampuf.github.io/GraphvizOnline/

```
digraph SlabLayout {
    rankdir=LR;
    node [shape=record, fontname="Arial"];

    slab [
        label="{
            Slab Header |
            a |
            { Block 1 |
                { Header | a | nullptr }
            } | a |
            { Block 2 |
                { Header | a | *Block 1 }
            } | a |
            { Block 3 |
                { Header | a | *Block 2 }
            } | a |
            { Block 4 |
                { Header | a | *Block 3 }
            } | a
        }"
    ];
    
    note1 [shape=note, label="a for 'alignment'"];

    slab -> note1 [style=dashed];
}
```