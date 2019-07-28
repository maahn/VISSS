#!/usr/bin/env bash
PATH=/home/visss/Desktop/VISSS/
# Git: add and commit changes
cd $PATH && /usr/bin/git commit -a -m "daily crontab backup `date`"

#  0 1  *   *   * /home/visss/Desktop/VISSS/scripts/autocommit.sh 
