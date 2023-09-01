****************
August 2023 Update
****************

Documentation
=============

* Add :doc:`documentation on adaptive disabling of partial agregation </develop/aggregations>`.
* Add :doc:`documentation on Aggregate::toIntermediate() API </develop/aggregate-functions>`.
* Add :doc:`documentation on connectors </develop/connectors>`.

Core Library
============

* Add support for remote functions. :pr:`6253`
* Add support for accessing sub-fields by index.
* Add support for DECIMAL in UnsafeRowSerializer.
* Add checks for verifying internal state of a vector.
* Enforce coalesce and switch expression to ensure that all inputs are of same type.
* Fixes for handling exceptions in try_cast from JSON.


Presto Functions
================

* Add :func:`split_to_map` , :func:`array_remove`, :func:`laplace_cdf`, :func:`date_format`, :func: `format_datetime` functions.
* Add :func:`gamma_cdf`, :func:`poisson_cdf` , `last_day_of_month` and 2 argument :func:`trim` functions.
* Add :func:`wilson_interval_lower` , :func:`wilson_interval_upper` functions.
* Optimize :func:`array_constructor` function.


Spark Functions
===============

* Add :spark:func:`date_add`, :spark:func:`date_sub`, :spark:func:`pmod` , :spark:func:`week_of_year`
* Add :spark:func:`max_by` , :spark:func:`min_by` spark aggregate functions.
* Add :spark:func:`dayofyear` , :spark:func:`dayofmonth`, :spark:func:`dayofweek` functions.
* Add :spark:func:`row_number` window function.


Hive Connector
==============

* Add support for writing to `Google Cloud Storage <https://cloud.google.com/storage>`_. :pr:`5685`
* Add support for writing data to HDFS via INSERT INTO/CTAS.


Performance and Correctness
===========================

* Add spilling performance benchmark. :pr:`6071`
* Added OOM protection for table creation during hash builds.
* Enhance Aggregation Fuzzer to test abandon-partial-agg code paths.
* Optimize single partition spilling.


Build System
============

* Add longer fuzzer runs when pull request has changes to sensitive files.

Credits
=======

Alexander Yermolovich, Amit Dutta, Ann Rose Benny, Arun D. Panicker, Ashwin Krishna Kumar, Austin Dickey, Bikramjeet Vig, Chengcheng Jin, Christian Zentgraf, Daniel Munoz, David Tolnay, Deepak Majeti, Ebe Janchivdorj, Ge Gao, Giuseppe Ottaviano, Harsha Rastogi, Hongze Zhang, Jacob Wujciak-Jens, Jia Ke, Jialiang Tan, Jimmy Lu, Karteek Murthy Samba Murthy, Karteekmurthys, Ke, Kevin Wilfong, Krishna Pai, Laith Sakka, Luca Niccolini, Ma-Jian1, Mack Ward, Mahadevuni Naveen Kumar, Masha Basmanova, Mike Lui, Nick Terrell, Open Source Bot, Orri Erling, Patrick Sullivan, Pedro Eugenio Rocha Pedreira, Pedro Pedreira, Pramod, Pranjal Shankhdhar, Richard Barnes, Rong Ma, Sandino Flores, Sanjiban Sengupta, Shiyu Gan, Wei He, Zac, Zhe Wan, aditi-pandit, duanmeng, ericyuliu, generatedunixname89002005287564, generatedunixname89002005325676, jackylee-ch, leesf, root, rui-mo, wangxinshuo.db, wypb, xiaoxmeng, yingsu00, yiweiHeOSS, zhejiangxiaomai, 陈旭