.class LTestSync;
.super Ljava/lang/Object;
.source "Main.java"


# direct methods
.method constructor <init>()V
    .registers 1

    .prologue
    .line 6
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V

    return-void
.end method

.method public static run()V
    # v0-v2 were generated by javac+dx for the original src code, keeping them.
    # v10..v19 are for tracking, aliasing and manipulating the first lock.
    # v20..v29 are for tracking, aliasing and manipulating the second lock.
    .registers 30

    .prologue
    .line 8
    const-string v1, "First"

    .line 9
    const-string v2, "Second"

    move-object v10, v1
    const v1, 0x1

    .line 10
    monitor-enter v10

    # Introduce a range of dead copies.
    move-object v11, v10
    move-object v12, v10
    move-object v13, v10
    move-object v14, v10
    move-object v15, v10
    move-object/16 v16, v10
    move-object/16 v17, v10
    move-object/16 v18, v10

    # Introduce a copy that we'll use for unlock.
    move-object/16 v19, v10

    # Clobber the original alias.
    const v10, 0x3

    move-object/16 v20, v2
    const v2, 0x2

    .line 11
    :try_start_b
    monitor-enter v20
    :try_end_c

    # Introduce a range of dead copies.
    move-object/16 v21, v20
    move-object/16 v22, v20
    move-object/16 v23, v20
    move-object/16 v24, v20
    move-object/16 v25, v20
    move-object/16 v26, v20
    move-object/16 v27, v20

    # Introduce another copy that we will hold live.
    move-object/16 v28, v20

    # Clobber the original alias.
    const v20, 0x5

    # Introduce another copy that we'll use for unlock.
    move-object/16 v29, v28

    .catchall {:try_start_b .. :try_end_c} :catchall_15

    .line 12
    :try_start_c
    invoke-static/range { v28 }, LMain;->run(Ljava/lang/Object;)V

    .line 13
    monitor-exit v29
    :try_end_10
    .catchall {:try_start_c .. :try_end_10} :catchall_12

    .line 14
    :try_start_10
    monitor-exit v19
    :try_end_11
    .catchall {:try_start_10 .. :try_end_11} :catchall_15

    .line 15
    return-void

    .line 13
    :catchall_12
    move-exception v0

    :try_start_13
    monitor-exit v29
    :try_end_14
    .catchall {:try_start_13 .. :try_end_14} :catchall_12

    :try_start_14
    throw v0

    .line 14
    :catchall_15
    move-exception v0

    monitor-exit v19
    :try_end_17
    .catchall {:try_start_14 .. :try_end_17} :catchall_15

    throw v0
.end method
