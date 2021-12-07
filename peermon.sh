#!/bin/bash
peers='
13.53.194.114
169.54.2.154
198.11.206.11
'

declare -A pids

for peer in $peers
do
    ./peermon $peer 51235 no-stats messages 2,3,30-41 >& $peer.out &
    pids[$peer]=$!
done

while [ 1 ]
do
    for peer in "${!pids[@]}"
    do
        j=`ps -p ${pids[$peer]} -o comm=`
        echo "$peer ${pids[$peer]} $j"
        # re-start if exited
        if [ "$j" = "" ]
        then
            echo "restarting $peer"
            ./peermon $peer 51235 no-stats &>> peer.out &
            pids[$peer]=$!
        fi
    done
    sleep 5
done
