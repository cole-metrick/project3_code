/*
 * CS 1652 Project 3 
 * (c) Amy Babay, 2022
 * (c) <Student names here>
 * 
 * Computer Science Department
 * University of Pittsburgh
 */

#include <stdlib.h>
#include "spu_alarm.h"
#include "edge_list.h"

/* Add edge to edge list.
 *     returns pointer to edge if added successfully, NULL if list is full.
 *   Note that this functions creates a new edge struct and copies the edge
 *   param passed in into it */
struct edge * add_edge_to_list(struct edge_list *list, struct edge e)
{
    if (list->num_edges == MAX_EDGES) {
        Alarm(DEBUG, "Edge list full. Not adding new edge\n");
        return NULL;
    }

    list->edges[list->num_edges] = calloc(1, sizeof(struct edge));
    *(list->edges[list->num_edges]) = e;
    list->num_edges++;

    return list->edges[list->num_edges - 1];
}
