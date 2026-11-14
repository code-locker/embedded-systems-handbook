# How to learn so you never forget

Reading something feels like learning, but most of it is gone within a week.
Memory research gives a small set of techniques that actually work. This repo
is built around them.

## The four rules

### 1. Retrieve, don't re-read (active recall)
Close the README and *pull the idea out of your head*: explain it out loud,
draw the diagram, write the code from a blank file. The effort of recalling
is what makes a memory strong. Every lesson ends with **Self-test questions**
and a **Remember forever** card for this purpose.

### 2. Space it out (spaced repetition)
Review a topic just as you are about to forget it. Use this schedule after
you first finish a lesson:

| Review | When | What to do (≈10–20 min) |
|---|---|---|
| R1 | next day | Recall card from memory, then check it |
| R2 | +3 days | Rewrite the core program from a blank file |
| R3 | +1 week | Answer the self-test questions out loud |
| R4 | +3 weeks | Solve one exercise you have not done |
| R5 | +2 months | Explain the topic to someone (or to a rubber duck) |

If a review goes badly, restart the schedule from R1 for that topic. Track it
in [ROADMAP.md](ROADMAP.md).

### 3. Build it yourself, then break it (generation + elaboration)
Running the given code is step one. Then:
- **Change** it: add an operation, change the data type, make it generic.
- **Break** it on purpose: remove a `free`, go one past the end, forget a
  `gst_object_unref`. Watch what ASan, gdb or `GST_DEBUG` tell you. An error
  you have caused yourself is one you will recognise forever.
- **Predict** the output before running, and explain any difference.

### 4. Connect it to something real (elaboration)
Each README has a **Where it is used** section: the Linux kernel's `list_head`
and red-black trees, ring buffers in UART drivers, Dijkstra in OSPF routing,
Huffman in JPEG, GStreamer pipelines as graphs. Linking a new idea to
something you already know gives it a place to live in your memory.

## The study loop for one lesson (≈2–4 hours)

```
 read README (mental model + diagrams)
        │
        ▼
 run every program:  make run P=...
        │
        ▼
 step through the trickiest one in gdb  (break, next, print, display)
        │
        ▼
 modify + break it on purpose (see rule 3)
        │
        ▼
 close everything → rewrite the core from memory → compare
        │
        ▼
 do 2–3 exercises → write the recall card in your own words
        │
        ▼
 add the review dates to ROADMAP.md
```

## Tips that matter

- **Draw memory.** For anything with pointers, draw boxes and arrows on paper
  *before* coding. Most pointer bugs are wrong pictures.
- **Keep a "mistakes log"** (`notes/mistakes.md` in your fork): every bug you
  hit, its cause, and how you found it. Re-read it before interviews.
- **Interleave.** After the basics, mix topics in a session (one graph
  problem, one DP problem, one GStreamer exercise) instead of doing 10 of the
  same kind in a row. It is harder, and that is why it works.
- **Sleep.** Memory is consolidated during sleep. Two 1-hour sessions on two
  days beat one 2-hour session.
