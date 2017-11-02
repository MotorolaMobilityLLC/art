/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.ahat.heapdump;

import com.android.ahat.proguard.ProguardMap;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class Site implements Diffable<Site> {
  // The site that this site was directly called from.
  // mParent is null for the root site.
  private Site mParent;

  private String mMethodName;
  private String mSignature;
  private String mFilename;
  private int mLineNumber;

  // A unique id to identify this site with. The id is chosen based on a
  // depth first traversal of the complete site tree, which gives it the
  // following desired properties:
  // * The id can easily be represented in a URL.
  // * The id is determined by the hprof file, so that the same id can be used
  //   across different instances for viewing the same hprof file.
  // * A binary search can be used to find a site by id from the root site in
  //   log time.
  //
  // The id is set by prepareForUse after the complete site tree is constructed.
  private long mId = -1;

  // The total size of objects allocated in this site (including child sites),
  // organized by heap index. Computed as part of prepareForUse.
  private Size[] mSizesByHeap;

  // List of child sites.
  private List<Site> mChildren;

  // List of objects allocated at this site (not including child sites).
  private List<AhatInstance> mObjects;

  private List<ObjectsInfo> mObjectsInfos;
  private Map<AhatHeap, Map<AhatClassObj, ObjectsInfo>> mObjectsInfoMap;

  private Site mBaseline;

  public static class ObjectsInfo implements Diffable<ObjectsInfo> {
    public AhatHeap heap;
    public AhatClassObj classObj;   // May be null.
    public long numInstances;
    public Size numBytes;
    private ObjectsInfo baseline;

    /**
     * Construct a new, empty objects info for the given heap and class
     * combination.
     */
    public ObjectsInfo(AhatHeap heap, AhatClassObj classObj) {
      this.heap = heap;
      this.classObj = classObj;
      this.numInstances = 0;
      this.numBytes = Size.ZERO;
      this.baseline = this;
    }

    /**
     * Returns the name of the class this ObjectsInfo is associated with.
     */
    public String getClassName() {
      return classObj == null ? "???" : classObj.getName();
    }

    public void setBaseline(ObjectsInfo baseline) {
      this.baseline = baseline;
    }

    @Override public ObjectsInfo getBaseline() {
      return baseline;
    }

    @Override public boolean isPlaceHolder() {
      return false;
    }
  }

  /**
   * Construct a root site.
   */
  Site(String name) {
    this(null, name, "", "", 0);
  }

  private Site(Site parent, String method, String signature, String file, int line) {
    mParent = parent;
    mMethodName = method;
    mSignature = signature;
    mFilename = file;
    mLineNumber = line;
    mChildren = new ArrayList<Site>();
    mObjects = new ArrayList<AhatInstance>();
    mObjectsInfos = new ArrayList<ObjectsInfo>();
    mObjectsInfoMap = new HashMap<AhatHeap, Map<AhatClassObj, ObjectsInfo>>();
    mBaseline = this;
  }

  /**
   * Get a child site of this site.
   * Returns the site at which the instance was allocated.
   * @param frames - The list of frames in the stack trace, starting with the
   *                 inner-most frame. May be null, in which case this site is
   *                 returned.
   */
  Site getSite(ProguardMap.Frame[] frames) {
    return frames == null ? this : getSite(this, frames);
  }

  private static Site getSite(Site site, ProguardMap.Frame[] frames) {
    for (int s = frames.length - 1; s >= 0; --s) {
      ProguardMap.Frame frame = frames[s];
      Site child = null;
      for (int i = 0; i < site.mChildren.size(); i++) {
        Site curr = site.mChildren.get(i);
        if (curr.mLineNumber == frame.line
            && curr.mMethodName.equals(frame.method)
            && curr.mSignature.equals(frame.signature)
            && curr.mFilename.equals(frame.filename)) {
          child = curr;
          break;
        }
      }
      if (child == null) {
        child = new Site(site, frame.method, frame.signature,
            frame.filename, frame.line);
        site.mChildren.add(child);
      }
      site = child;
    }
    return site;
  }

  /**
   * Add an instance allocated at this site.
   */
  void addInstance(AhatInstance inst) {
    mObjects.add(inst);
  }

  /**
   * Prepare this and all child sites for use.
   * Recomputes site ids, sizes, ObjectInfos for this and all child sites.
   * This should be called after the sites tree has been formed and after
   * dominators computation has been performed to ensure only reachable
   * objects are included in the ObjectsInfos.
   *
   * @param id - The smallest id that is allowed to be used for this site or
   * any of its children.
   * @param numHeaps - The number of heaps in the heap dump.
   * @return An id larger than the largest id used for this site or any of its
   * children.
   */
  long prepareForUse(long id, int numHeaps) {
    mId = id++;

    // Count up the total sizes by heap.
    mSizesByHeap = new Size[numHeaps];
    for (int i = 0; i < numHeaps; ++i) {
      mSizesByHeap[i] = Size.ZERO;
    }

    // Add all reachable objects allocated at this site.
    for (AhatInstance inst : mObjects) {
      if (inst.isStronglyReachable()) {
        AhatHeap heap = inst.getHeap();
        Size size = inst.getSize();
        ObjectsInfo info = getObjectsInfo(heap, inst.getClassObj());
        info.numInstances++;
        info.numBytes = info.numBytes.plus(size);
        mSizesByHeap[heap.getIndex()] = mSizesByHeap[heap.getIndex()].plus(size);
      }
    }

    // Add objects allocated in child sites.
    for (Site child : mChildren) {
      id = child.prepareForUse(id, numHeaps);
      for (ObjectsInfo childInfo : child.mObjectsInfos) {
        ObjectsInfo info = getObjectsInfo(childInfo.heap, childInfo.classObj);
        info.numInstances += childInfo.numInstances;
        info.numBytes = info.numBytes.plus(childInfo.numBytes);
      }
      for (int i = 0; i < numHeaps; ++i) {
        mSizesByHeap[i] = mSizesByHeap[i].plus(child.mSizesByHeap[i]);
      }
    }
    return id;
  }

  // Get the size of a site for a specific heap.
  public Size getSize(AhatHeap heap) {
    return mSizesByHeap[heap.getIndex()];
  }

  /**
   * Collect the objects allocated under this site, optionally filtered by
   * heap name or class name. Includes objects allocated in children sites.
   * @param heapName - The name of the heap the collected objects should
   *                   belong to. This may be null to indicate objects of
   *                   every heap should be collected.
   * @param className - The name of the class the collected objects should
   *                    belong to. This may be null to indicate objects of
   *                    every class should be collected.
   * @param objects - Out parameter. A collection of objects that all
   *                  collected objects should be added to.
   */
  public void getObjects(String heapName, String className, Collection<AhatInstance> objects) {
    for (AhatInstance inst : mObjects) {
      if ((heapName == null || inst.getHeap().getName().equals(heapName))
          && (className == null || inst.getClassName().equals(className))) {
        objects.add(inst);
      }
    }

    // Recursively visit children. Recursion should be okay here because the
    // stack depth is limited by a reasonable amount (128 frames or so).
    for (Site child : mChildren) {
      child.getObjects(heapName, className, objects);
    }
  }

  /**
   * Returns the ObjectsInfo at this site for the given heap and class
   * objects. Creates a new empty ObjectsInfo if none existed before.
   */
  ObjectsInfo getObjectsInfo(AhatHeap heap, AhatClassObj classObj) {
    Map<AhatClassObj, ObjectsInfo> classToObjectsInfo = mObjectsInfoMap.get(heap);
    if (classToObjectsInfo == null) {
      classToObjectsInfo = new HashMap<AhatClassObj, ObjectsInfo>();
      mObjectsInfoMap.put(heap, classToObjectsInfo);
    }

    ObjectsInfo info = classToObjectsInfo.get(classObj);
    if (info == null) {
      info = new ObjectsInfo(heap, classObj);
      mObjectsInfos.add(info);
      classToObjectsInfo.put(classObj, info);
    }
    return info;
  }

  public List<ObjectsInfo> getObjectsInfos() {
    return mObjectsInfos;
  }

  // Get the combined size of the site for all heaps.
  public Size getTotalSize() {
    Size total = Size.ZERO;
    for (Size size : mSizesByHeap) {
      total = total.plus(size);
    }
    return total;
  }

  /**
   * Return the site this site was called from.
   * Returns null for the root site.
   */
  public Site getParent() {
    return mParent;
  }

  public String getMethodName() {
    return mMethodName;
  }

  public String getSignature() {
    return mSignature;
  }

  public String getFilename() {
    return mFilename;
  }

  public int getLineNumber() {
    return mLineNumber;
  }

  /**
   * Returns the unique id of this site.
   */
  public long getId() {
    return mId;
  }

  /**
   * Find the child site with the given id.
   * Returns null if no such site was found.
   */
  public Site findSite(long id) {
    if (id == mId) {
      return this;
    }

    // Binary search over the children to find the right child to search in.
    int start = 0;
    int end = mChildren.size();
    while (start < end) {
      int mid = start + ((end - start) / 2);
      Site midSite = mChildren.get(mid);
      if (id < midSite.mId) {
        end = mid;
      } else if (mid + 1 == end) {
        // This is the last child we could possibly find the desired site in,
        // so search in this child.
        return midSite.findSite(id);
      } else if (id < mChildren.get(mid + 1).mId) {
        // The desired site has an id between this child's id and the next
        // child's id, so search in this child.
        return midSite.findSite(id);
      } else {
        start = mid + 1;
      }
    }
    return null;
  }

  /**
   * Returns an unmodifiable list of this site's immediate children.
   */
  public List<Site> getChildren() {
    return Collections.unmodifiableList(mChildren);
  }

  void setBaseline(Site baseline) {
    mBaseline = baseline;
  }

  @Override public Site getBaseline() {
    return mBaseline;
  }

  @Override public boolean isPlaceHolder() {
    return false;
  }
}
